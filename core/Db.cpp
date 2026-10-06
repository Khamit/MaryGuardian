#include "Db.hpp"
#include "Extract.hpp"
#include "TextUtil.hpp"
#include <sqlite3.h>
#include <iostream>

namespace mary {
using json = nlohmann::json;

namespace {

// Сколько разных SQL держим подготовленными. Динамические запросы (cleanup) не должны раздувать кеш бесконечно.
constexpr size_t kMaxCachedStatements = 256;

void bindAll(sqlite3_stmt* st, const std::vector<json>& p) {
    for (size_t i = 0; i < p.size(); ++i) {
        const int k = (int)i + 1;
        const json& v = p[i];
        if (v.is_null()) sqlite3_bind_null(st, k);
        else if (v.is_boolean()) sqlite3_bind_int(st, k, v.get<bool>() ? 1 : 0);
        else if (v.is_number_integer()) sqlite3_bind_int64(st, k, v.get<long long>());
        else if (v.is_number()) sqlite3_bind_double(st, k, v.get<double>());
        else if (v.is_string()) {
            const std::string& s = v.get_ref<const std::string&>();
            sqlite3_bind_text(st, k, s.data(), (int)s.size(), SQLITE_TRANSIENT);
        } else {
            const std::string s = v.dump();
            sqlite3_bind_text(st, k, s.data(), (int)s.size(), SQLITE_TRANSIENT);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// RowView
// ---------------------------------------------------------------------------
bool RowView::isNull(int col) const { return sqlite3_column_type(st_, col) == SQLITE_NULL; }
long long RowView::i64(int col) const { return (long long)sqlite3_column_int64(st_, col); }
std::string RowView::text(int col) const {
    const unsigned char* t = sqlite3_column_text(st_, col);
    if (!t) return std::string();
    return std::string((const char*)t, (size_t)sqlite3_column_bytes(st_, col));
}

// RAII: пока statement выполняется — кеш не вычищаем; по выходу всегда reset (иначе висит read-транзакция WAL).
struct Db::Scope {
    Db& db;
    sqlite3_stmt* st;
    Scope(Db& d, sqlite3_stmt* s) : db(d), st(s) { ++db.busy_; }
    ~Scope() { sqlite3_reset(st); --db.busy_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

// ---------------------------------------------------------------------------
// Жизненный цикл
// ---------------------------------------------------------------------------
Db::Db(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::cerr << "[Db] open failed: " << (db_ ? sqlite3_errmsg(db_) : "?") << "\n";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return;
    }
    sqlite3_busy_timeout(db_, 3000);
    init();
}

Db::~Db() {
    if (!db_) return;
    dropCache();   // иначе sqlite3_close вернёт SQLITE_BUSY и утечёт
    sqlite3_exec(db_, "PRAGMA optimize", nullptr, nullptr, nullptr);
    sqlite3_close(db_);
}

void Db::dropCache() {
    for (auto& kv : cache_) sqlite3_finalize(kv.second);
    cache_.clear();
}

sqlite3_stmt* Db::acquire(const std::string& sql) {
    auto it = cache_.find(sql);
    if (it != cache_.end()) {
        sqlite3_reset(it->second);
        sqlite3_clear_bindings(it->second);
        return it->second;
    }
    if (cache_.size() >= kMaxCachedStatements && busy_ == 0) dropCache();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), (int)sql.size(), &st, nullptr) != SQLITE_OK) {
        std::cerr << "[Db] prepare: " << sqlite3_errmsg(db_) << "\n";
        return nullptr;
    }
    if (!st) return nullptr;   // пустой SQL
    cache_.emplace(sql, st);
    return st;
}

void Db::init() {
    // WAL + synchronous=NORMAL: commit не делает fsync (он только на checkpoint). Процесс может упасть
    // без потерь; при потере питания теряются лишь последние транзакции, база остаётся целой.
    // Индекс полностью восстанавливается из inbox, поэтому цена потери минимальна.
    const char* pragmas = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA temp_store=MEMORY;
PRAGMA cache_size=-32768;
PRAGMA foreign_keys=ON;
)SQL";
    const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS companies(
  id INTEGER PRIMARY KEY, canonical TEXT UNIQUE NOT NULL, display TEXT NOT NULL, confirmed INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS collections(
  id INTEGER PRIMARY KEY, name TEXT NOT NULL, auto_type TEXT, keywords TEXT NOT NULL DEFAULT '', sort INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS documents(
  id INTEGER PRIMARY KEY, sha256 TEXT UNIQUE NOT NULL, filename TEXT NOT NULL, size INTEGER, mtime INTEGER,
  present INTEGER NOT NULL DEFAULT 1,
  status TEXT NOT NULL, doc_type TEXT, doc_date TEXT, doc_number TEXT, amount_minor INTEGER, currency TEXT,
  company_id INTEGER REFERENCES companies(id), collection_id INTEGER REFERENCES collections(id),
  summary TEXT, expires TEXT, expires_basis TEXT, pii INTEGER NOT NULL DEFAULT 0,
  confidence REAL, source TEXT, user_locked INTEGER NOT NULL DEFAULT 0,
  classifier_ver INTEGER NOT NULL DEFAULT 0, indexed_at TEXT);
CREATE INDEX IF NOT EXISTS idx_docs_name ON documents(filename, size, mtime);
CREATE INDEX IF NOT EXISTS idx_docs_status ON documents(status);
CREATE TABLE IF NOT EXISTS ocr_text(doc_id INTEGER PRIMARY KEY REFERENCES documents(id), text TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS profile(key TEXT PRIMARY KEY, value TEXT NOT NULL);
-- Результат LLM по (модель + промпт): одну и ту же работу модель не делает дважды.
CREATE TABLE IF NOT EXISTS llm_cache(
  k TEXT PRIMARY KEY, doc_type TEXT, company TEXT, summary TEXT) WITHOUT ROWID;
)SQL";
    // Индексы под реальные запросы UI: счётчики коллекций/компаний и порядок списка «по дате».
    // Отдельным вызовом: если что-то не поддерживается, базовая схема всё равно создана.
    const char* indexes = R"SQL(
CREATE INDEX IF NOT EXISTS idx_docs_coll ON documents(collection_id, present);
CREATE INDEX IF NOT EXISTS idx_docs_comp ON documents(company_id, present);
CREATE INDEX IF NOT EXISTS idx_docs_order ON documents(COALESCE(doc_date,'0000') DESC, id DESC);
)SQL";

    char* err = nullptr;
    if (sqlite3_exec(db_, pragmas, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[Db] pragma error: " << (err ? err : "?") << "\n";
        sqlite3_free(err);
        err = nullptr;
    }
    if (sqlite3_exec(db_, schema, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[Db] schema error: " << (err ? err : "?") << "\n";
        sqlite3_free(err);
        return;
    }
    if (sqlite3_exec(db_, indexes, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[Db] index warning: " << (err ? err : "?") << "\n";
        sqlite3_free(err);
    }
    // Встроенные коллекции — по одной на тип из таблицы в Extract.hpp. Идемпотентно: недостающие добавляются и в
    // уже существующую базу, переименования и пользовательские коллекции не затрагиваются, порядок встроенных стабилен.
    batch([&]() -> bool {
        exec("UPDATE collections SET sort=sort+100 WHERE auto_type IS NULL AND sort<100");   // свои коллекции — после встроенных
        int i = 0;
        for (const auto& dt : extract::docTypes()) {
            exec("INSERT INTO collections(name,auto_type,sort) SELECT ?,?,? WHERE NOT EXISTS (SELECT 1 FROM collections WHERE auto_type=?)",
                 {dt.collection, dt.type, i, dt.type});
            exec("UPDATE collections SET sort=? WHERE auto_type=?", {i, dt.type});
            ++i;
        }
        return true;
    });
}

// ---------------------------------------------------------------------------
// Запросы
// ---------------------------------------------------------------------------
json Db::query(const std::string& sql, const std::vector<json>& params) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    json rows = json::array();
    if (!db_) return rows;
    sqlite3_stmt* st = acquire(sql);
    if (!st) return rows;
    Scope sc(*this, st);
    bindAll(st, params);

    const int n = sqlite3_column_count(st);
    std::vector<std::string> names((size_t)n);              // имена колонок — один раз, а не на каждую строку
    for (int c = 0; c < n; ++c) {
        const char* nm = sqlite3_column_name(st, c);
        names[(size_t)c] = nm ? nm : "";
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        json r = json::object();
        for (int c = 0; c < n; ++c) {
            const std::string& name = names[(size_t)c];
            switch (sqlite3_column_type(st, c)) {
                case SQLITE_INTEGER: r[name] = (long long)sqlite3_column_int64(st, c); break;
                case SQLITE_FLOAT:   r[name] = sqlite3_column_double(st, c); break;
                case SQLITE_NULL:    r[name] = nullptr; break;
                default: {
                    const unsigned char* t = sqlite3_column_text(st, c);
                    r[name] = t ? std::string((const char*)t, (size_t)sqlite3_column_bytes(st, c)) : std::string();
                }
            }
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

long long Db::exec(const std::string& sql, const std::vector<json>& params, long long* last_id) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (!db_) return -1;
    sqlite3_stmt* st = acquire(sql);
    if (!st) return -1;
    Scope sc(*this, st);
    bindAll(st, params);

    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {}
    if (rc != SQLITE_DONE) {
        std::cerr << "[Db] exec: " << sqlite3_errmsg(db_) << "\n";
        return -1;
    }
    const long long changes = sqlite3_changes(db_);
    if (last_id) *last_id = sqlite3_last_insert_rowid(db_);

    // Любая запись в эти таблицы делает RAM-копии недействительными. Централизованно, чтобы не забыть.
    if (sql.find("collections") != std::string::npos) collLoaded_ = false;
    if (sql.find("companies") != std::string::npos) companyCache_.clear();
    return changes;
}

void Db::each(const std::string& sql, const std::vector<json>& params,
              const std::function<void(const RowView&)>& fn) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (!db_) return;
    sqlite3_stmt* st = acquire(sql);
    if (!st) return;
    Scope sc(*this, st);
    bindAll(st, params);
    RowView rv(st);
    while (sqlite3_step(st) == SQLITE_ROW) fn(rv);
}

bool Db::batch(const std::function<bool()>& fn) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (!db_) return false;
    if (txDepth_ > 0) {                       // уже внутри транзакции
        try { return fn(); } catch (...) { return false; }
    }
    char* err = nullptr;
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[Db] begin: " << (err ? err : "?") << "\n";
        sqlite3_free(err);
        return false;
    }
    ++txDepth_;
    bool ok = false;
    try { ok = fn(); }
    catch (const std::exception& e) { std::cerr << "[Db] batch: " << e.what() << "\n"; ok = false; }
    catch (...) { ok = false; }
    --txDepth_;
    if (ok && sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "[Db] commit: " << sqlite3_errmsg(db_) << "\n";
        ok = false;
    }
    if (!ok) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    return ok;
}

// ---------------------------------------------------------------------------
// Компании и коллекции (кеш в RAM)
// ---------------------------------------------------------------------------
long long Db::companyId(const std::string& display, bool confirm) {
    std::string clean = text::cut(text::oneLine(text::trim(display)), 80);
    std::string canon = extract::normalizeCompany(clean);
    if (canon.empty()) return 0;
    std::lock_guard<std::recursive_mutex> lk(mu_);

    auto it = companyCache_.find(canon);
    if (it != companyCache_.end()) {
        const long long id = it->second.id;
        const bool conf = it->second.confirmed;
        if (confirm && !conf) {
            exec("UPDATE companies SET confirmed=1 WHERE id=?", {id});   // сбрасывает кеш
            companyCache_[canon] = CompanyHit{id, true};
        }
        return id;
    }

    auto r = query("SELECT id, confirmed FROM companies WHERE canonical=?", {canon});
    if (!r.empty()) {
        const long long id = r[0]["id"].get<long long>();
        bool conf = r[0]["confirmed"].get<long long>() != 0;
        if (confirm && !conf) {
            exec("UPDATE companies SET confirmed=1 WHERE id=?", {id});
            conf = true;
        }
        companyCache_[canon] = CompanyHit{id, conf};
        return id;
    }
    long long id = 0;
    exec("INSERT INTO companies(canonical,display,confirmed) VALUES(?,?,?)", {canon, clean, confirm ? 1 : 0}, &id);
    if (id) companyCache_[canon] = CompanyHit{id, confirm};
    return id;
}

void Db::loadCollections() {
    collKw_.clear();
    collByType_.clear();
    collOther_ = 0;
    auto cols = query("SELECT id, keywords, auto_type FROM collections ORDER BY sort, id");
    for (const auto& c : cols) {
        const long long id = c["id"].get<long long>();
        if (c["auto_type"].is_string()) {
            const std::string t = c["auto_type"].get<std::string>();
            auto it = collByType_.find(t);
            if (it == collByType_.end() || id < it->second) collByType_[t] = id;   // как LIMIT 1 по rowid
        }
        if (!c["keywords"].is_string()) continue;
        const std::string kws = text::lower(c["keywords"].get<std::string>());
        CollKw e{id, {}};
        size_t start = 0;
        while (start <= kws.size()) {
            const size_t comma = kws.find(',', start);
            std::string kw = text::trim(kws.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!kw.empty()) e.kws.push_back(std::move(kw));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (!e.kws.empty()) collKw_.push_back(std::move(e));
    }
    auto oth = collByType_.find("other");
    collOther_ = oth == collByType_.end() ? 0 : oth->second;
    collLoaded_ = true;
}

long long Db::pickCollection(const std::string& doc_type, const std::string& hay) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (!collLoaded_) loadCollections();
    for (const auto& c : collKw_)
        for (const auto& kw : c.kws)
            if (hay.find(kw) != std::string::npos) return c.id;
    auto it = collByType_.find(doc_type);
    if (it != collByType_.end()) return it->second;
    return collOther_;
}

} // namespace mary