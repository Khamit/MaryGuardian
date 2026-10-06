#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

struct sqlite3;
struct sqlite3_stmt;

namespace mary {

// Доступ к текущей строке результата без построения JSON (для больших выборок).
class RowView {
public:
    explicit RowView(sqlite3_stmt* st) : st_(st) {}
    bool isNull(int col) const;
    long long i64(int col) const;
    std::string text(int col) const;
private:
    sqlite3_stmt* st_;
};

// Тонкая обёртка над SQLite. Все запросы параметризованы, строки возвращаются как JSON.
//  * prepared-statements кешируются: каждый SQL разбирается один раз;
//  * batch() — много записей в одной транзакции (один commit вместо N);
//  * each() — потоковое чтение больших выборок без JSON;
//  * коллекции и компании кешируются в RAM и сбрасываются автоматически при любой записи в их таблицы.
class Db {
public:
    explicit Db(const std::string& path);
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    bool ok() const { return db_ != nullptr; }

    nlohmann::json query(const std::string& sql, const std::vector<nlohmann::json>& params = {});
    // Возвращает число изменённых строк (-1 при ошибке). last_id — rowid последней вставки.
    long long exec(const std::string& sql, const std::vector<nlohmann::json>& params = {}, long long* last_id = nullptr);

    // Построчное чтение. Внутри fn НЕЛЬЗЯ обращаться к Db (держим тот же statement).
    void each(const std::string& sql, const std::vector<nlohmann::json>& params,
              const std::function<void(const RowView&)>& fn);

    // Выполнить fn в одной транзакции. fn вернула false или бросила исключение → ROLLBACK.
    // Вложенные вызовы безопасны: внутренний просто выполняется в рамках внешней транзакции.
    // Пока транзакция открыта, остальные потоки ждут (замок держится на всё время fn).
    bool batch(const std::function<bool()>& fn);

    // Найти или создать компанию (по нормализованному имени). confirm=true → подтверждена пользователем.
    long long companyId(const std::string& display, bool confirm);
    // Коллекция для документа: сначала ключевые слова пользователя, затем авто-тип.
    long long pickCollection(const std::string& doc_type, const std::string& haystack_lower);

private:
    struct Scope;
    struct CollKw { long long id; std::vector<std::string> kws; };
    struct CompanyHit { long long id; bool confirmed; };

    void init();
    sqlite3_stmt* acquire(const std::string& sql);   // из кеша или новый; mu_ должен быть захвачен
    void dropCache();
    void loadCollections();                          // mu_ должен быть захвачен

    sqlite3* db_ = nullptr;
    std::recursive_mutex mu_;

    std::unordered_map<std::string, sqlite3_stmt*> cache_;
    int busy_ = 0;       // сколько statements сейчас выполняется (кеш в это время не чистим)
    int txDepth_ = 0;

    // Коллекции: ключевые слова уже разобраны, авто-типы уже сопоставлены.
    bool collLoaded_ = false;
    std::vector<CollKw> collKw_;
    std::unordered_map<std::string, long long> collByType_;
    long long collOther_ = 0;

    // Компании: canonical → (id, confirmed).
    std::unordered_map<std::string, CompanyHit> companyCache_;
};

} // namespace mary