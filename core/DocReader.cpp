#include "DocReader.hpp"
#include "PdfToolEngine.hpp"
#include "Safety.hpp"
#include "TextUtil.hpp"
#include <mupdf/fitz.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>
#include <iostream>

namespace mary { namespace docread {
namespace {

// ============================================================
// ZIP через MuPDF (без новых зависимостей)
// ============================================================
class Zip {
public:
    explicit Zip(const std::string& path) {
        ctx_ = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        if (!ctx_) return;
        fz_try(ctx_) { arch_ = fz_open_archive(ctx_, path.c_str()); }
        fz_catch(ctx_) { arch_ = nullptr; }
    }
    ~Zip() {
        if (arch_) fz_drop_archive(ctx_, arch_);
        if (ctx_) fz_drop_context(ctx_);
    }
    Zip(const Zip&) = delete;
    Zip& operator=(const Zip&) = delete;

    bool ok() const { return arch_ != nullptr; }

    bool has(const std::string& name) const {
        int r = 0;
        fz_var(r);
        fz_try(ctx_) { r = fz_has_archive_entry(ctx_, arch_, name.c_str()); }
        fz_catch(ctx_) { r = 0; }
        return r != 0;
    }

    std::string read(const std::string& name) const {
        std::string out;
        fz_buffer* buf = nullptr;
        fz_var(buf);
        fz_try(ctx_) {
            buf = fz_read_archive_entry(ctx_, arch_, name.c_str());
            if (buf) {
                unsigned char* d = nullptr;
                size_t n = fz_buffer_storage(ctx_, buf, &d);
                out.assign((const char*)d, n);
            }
        }
        fz_always(ctx_) { if (buf) fz_drop_buffer(ctx_, buf); }
        fz_catch(ctx_) { out.clear(); }
        return out;
    }

    // Имена записей с префиксом/суффиксом, в «естественном» порядке (slide2 < slide10).
    std::vector<std::string> list(const std::string& prefix, const std::string& suffix) const {
        std::vector<std::string> v;
        int n = 0;
        fz_var(n);
        fz_try(ctx_) {
            n = fz_count_archive_entries(ctx_, arch_);
            for (int i = 0; i < n; ++i) {
                const char* e = fz_list_archive_entry(ctx_, arch_, i);
                if (!e) continue;
                std::string s = e;
                if (s.compare(0, prefix.size(), prefix) != 0) continue;
                if (s.size() < suffix.size() || s.compare(s.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
                v.push_back(s);
            }
        }
        fz_catch(ctx_) { /* вернём что успели */ }
        std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) {
            return a.size() != b.size() ? a.size() < b.size() : a < b;
        });
        return v;
    }

private:
    fz_context* ctx_ = nullptr;
    fz_archive* arch_ = nullptr;
};

// ============================================================
// Кодировки
// ============================================================
void putUtf8(std::string& o, unsigned long cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp <= 0x10FFFF) {
        o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
        o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
    }
}

bool validUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        size_t n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 99;
        if (n == 99 || i + 1 + n > s.size()) return false;
        for (size_t k = 1; k <= n; ++k) if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        i += n + 1;
    }
    return true;
}

std::string cp1251ToUtf8(const std::string& s) {
    std::string o;
    o.reserve(s.size() * 2);
    for (unsigned char c : s) {
        if (c < 0x80) { o += (char)c; continue; }
        unsigned long cp = '?';
        if (c >= 0xC0) cp = 0x410 + (c - 0xC0);
        else switch (c) {
            case 0xA8: cp = 0x401; break;   case 0xB8: cp = 0x451; break;
            case 0xB9: cp = 0x2116; break;  case 0x88: cp = 0x20AC; break;   // № и €
            case 0xA0: cp = 0xA0; break;    case 0xB0: cp = 0xB0; break;
            case 0xAB: cp = 0xAB; break;    case 0xBB: cp = 0xBB; break;
            case 0x96: cp = 0x2013; break;  case 0x97: cp = 0x2014; break;
            case 0x91: cp = 0x2018; break;  case 0x92: cp = 0x2019; break;
            case 0x93: cp = 0x201C; break;  case 0x94: cp = 0x201D; break;
            case 0x85: cp = 0x2026; break;
            default: break;
        }
        putUtf8(o, cp);
    }
    return o;
}

std::string utf16ToUtf8(const std::string& s, bool be, size_t from) {
    auto unit = [&](size_t i) -> unsigned {
        unsigned char a = (unsigned char)s[i], b = (unsigned char)s[i + 1];
        return be ? (unsigned)((a << 8) | b) : (unsigned)((b << 8) | a);
    };
    std::string o;
    for (size_t i = from; i + 1 < s.size(); i += 2) {
        unsigned u = unit(i);
        unsigned long cp = u;
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < s.size()) {
            unsigned lo = unit(i + 2);
            if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + (((unsigned long)u - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
        }
        putUtf8(o, cp);
    }
    return o;
}

std::string toUtf8(std::string raw) {
    auto b = [&](size_t i) { return (unsigned char)raw[i]; };
    if (raw.size() >= 2 && b(0) == 0xFF && b(1) == 0xFE) return utf16ToUtf8(raw, false, 2);
    if (raw.size() >= 2 && b(0) == 0xFE && b(1) == 0xFF) return utf16ToUtf8(raw, true, 2);
    if (raw.size() >= 3 && b(0) == 0xEF && b(1) == 0xBB && b(2) == 0xBF) raw.erase(0, 3);
    text::utf8TrimTail(raw);
    return validUtf8(raw) ? raw : cp1251ToUtf8(raw);   // не UTF-8 → считаем Windows-1251
}

// ============================================================
// Мини-парсер XML (достаточно для OOXML/ODF и простых выгрузок)
// ============================================================
std::string decodeEntities(const std::string& s) {
    if (s.find('&') == std::string::npos) return s;
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t e = s.find(';', i);
        if (e == std::string::npos || e - i > 9 || e == i + 1) { o += s[i]; continue; }
        std::string ent = s.substr(i + 1, e - i - 1);
        if (ent == "amp") o += '&';
        else if (ent == "lt") o += '<';
        else if (ent == "gt") o += '>';
        else if (ent == "quot") o += '"';
        else if (ent == "apos") o += '\'';
        else if (ent[0] == '#') {
            bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
            long cp = hex ? std::strtol(ent.c_str() + 2, nullptr, 16) : std::strtol(ent.c_str() + 1, nullptr, 10);
            if (cp > 0) putUtf8(o, (unsigned long)cp);
        } else { o += s[i]; continue; }
        i = e;
    }
    return o;
}

std::string localName(const std::string& n) {
    size_t c = n.rfind(':');
    return c == std::string::npos ? n : n.substr(c + 1);
}

std::string attr(const std::string& a, const std::string& key) {
    const std::string k = key + "=";
    for (size_t p = a.find(k); p != std::string::npos; p = a.find(k, p + 1)) {
        if (p != 0 && !std::isspace((unsigned char)a[p - 1]) && a[p - 1] != ':') continue;
        size_t q = p + k.size();
        if (q < a.size() && (a[q] == '"' || a[q] == '\'')) {
            size_t e = a.find(a[q], q + 1);
            if (e != std::string::npos) return decodeEntities(a.substr(q + 1, e - q - 1));
        }
    }
    return "";
}

// open(name, attrs), close(name), text(decoded). stop — досрочный выход.
template <class Open, class Close, class Text>
void walk(const std::string& x, const bool& stop, Open open, Close close, Text txt) {
    const size_t n = x.size();
    size_t i = 0;
    while (i < n && !stop) {
        if (x[i] != '<') {
            size_t e = x.find('<', i);
            if (e == std::string::npos) e = n;
            txt(decodeEntities(x.substr(i, e - i)));
            i = e;
            continue;
        }
        if (x.compare(i, 4, "<!--") == 0) { size_t e = x.find("-->", i + 4); i = e == std::string::npos ? n : e + 3; continue; }
        if (x.compare(i, 9, "<![CDATA[") == 0) {
            size_t e = x.find("]]>", i + 9);
            txt(x.substr(i + 9, (e == std::string::npos ? n : e) - (i + 9)));
            i = e == std::string::npos ? n : e + 3;
            continue;
        }
        if (x.compare(i, 2, "<?") == 0 || x.compare(i, 2, "<!") == 0) {
            size_t e = x.find('>', i);
            i = e == std::string::npos ? n : e + 1;
            continue;
        }
        size_t e = x.find('>', i);
        if (e == std::string::npos) break;
        const bool closing = i + 1 < n && x[i + 1] == '/';
        const bool self = e > i + 1 && x[e - 1] == '/';
        size_t b = i + (closing ? 2 : 1), ne = b;
        while (ne < e && !std::isspace((unsigned char)x[ne]) && x[ne] != '/') ++ne;
        std::string name = localName(x.substr(b, ne - b));
        if (closing) close(name);
        else {
            open(name, x.substr(ne, e - ne - (self ? 1 : 0)));
            if (self) close(name);
        }
        i = e + 1;
    }
}

// ============================================================
// docx / pptx: абзацы <p>, текст <t>, таблицы <tc>/<tr>
// ============================================================
void wordLike(const std::string& xml, std::string& out, size_t cap) {
    bool stop = false, inT = false;
    int cell = 0;
    walk(xml, stop,
        [&](const std::string& n, const std::string&) {
            if (n == "t") inT = true;
            else if (n == "tab") out += '\t';
            else if (n == "br" || n == "cr") out += '\n';
            else if (n == "tc") ++cell;
        },
        [&](const std::string& n) {
            if (n == "t") inT = false;
            else if (n == "p") out += cell ? " " : "\n";
            else if (n == "tc") { if (cell > 0) --cell; out += " | "; }
            else if (n == "tr") out += '\n';
            if (out.size() >= cap) stop = true;
        },
        [&](const std::string& t) { if (inT) out += t; });
}

std::string docxText(const Zip& z, size_t cap) {
    std::string out;
    wordLike(z.read("word/document.xml"), out, cap);
    return out;
}

std::string pptxText(const Zip& z, size_t cap) {
    std::string out;
    int k = 0;
    for (const auto& s : z.list("ppt/slides/slide", ".xml")) {
        if (out.size() >= cap) break;
        out += "--- Slide " + std::to_string(++k) + " ---\n";
        wordLike(z.read(s), out, cap);
    }
    return out;
}

// ============================================================
// odt / ods
// ============================================================
std::string odfText(const Zip& z, size_t cap) {
    std::string out;
    bool stop = false;
    int inP = 0, cell = 0;
    walk(z.read("content.xml"), stop,
        [&](const std::string& n, const std::string&) {
            if (n == "p" || n == "h") ++inP;
            else if (n == "s") out += ' ';
            else if (n == "tab") out += '\t';
            else if (n == "line-break") out += '\n';
            else if (n == "table-cell" || n == "covered-table-cell") ++cell;
        },
        [&](const std::string& n) {
            if (n == "p" || n == "h") { if (inP > 0) --inP; out += cell ? " " : "\n"; }
            else if (n == "table-cell" || n == "covered-table-cell") { if (cell > 0) --cell; out += " | "; }
            else if (n == "table-row") out += '\n';
            if (out.size() >= cap) stop = true;
        },
        [&](const std::string& t) { if (inP > 0) out += t; });
    return out;
}

// ============================================================
// xlsx: sharedStrings + styles (даты) + листы → строки с табами
// ============================================================
bool codeIsDate(std::string code) {
    code = text::lower(code);
    std::string s;
    bool q = false, br = false;
    for (char c : code) {
        if (c == '"') q = !q;
        else if (c == '[' && !q) br = true;
        else if (c == ']' && !q) br = false;
        else if (!q && !br) s += c;
    }
    return s.find('y') != std::string::npos || s.find('d') != std::string::npos;
}

bool isDateFmt(int id, const std::map<int, bool>& custom) {
    auto it = custom.find(id);
    if (it != custom.end()) return it->second;
    return (id >= 14 && id <= 22) || (id >= 27 && id <= 36) || (id >= 45 && id <= 47) || (id >= 50 && id <= 58);
}

// Excel serial (система 1900) → ISO. 25569 = 1970-01-01.
std::string isoFromSerial(long long serial) {
    long long z = serial - 25569 + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    char b[24];
    std::snprintf(b, sizeof b, "%04lld-%02u-%02u", y, m, d);
    return b;
}

int colIndex(const std::string& ref) {
    int c = 0;
    size_t i = 0;
    for (; i < ref.size() && std::isalpha((unsigned char)ref[i]); ++i)
        c = c * 26 + (std::toupper((unsigned char)ref[i]) - 'A' + 1);
    return i == 0 ? -1 : c - 1;
}

std::string xlsxText(const Zip& z, size_t cap) {
    std::vector<std::string> sst, names;
    std::vector<bool> dateXf;
    const bool never = false;

    {   // общие строки
        bool inSi = false, inT = false;
        int rph = 0;
        std::string cur;
        walk(z.read("xl/sharedStrings.xml"), never,
            [&](const std::string& n, const std::string&) {
                if (n == "si") { inSi = true; cur.clear(); }
                else if (n == "t") inT = true;
                else if (n == "rPh") ++rph;
            },
            [&](const std::string& n) {
                if (n == "t") inT = false;
                else if (n == "rPh") { if (rph > 0) --rph; }
                else if (n == "si") { sst.push_back(cur); inSi = false; }
            },
            [&](const std::string& t) { if (inSi && inT && rph == 0) cur += t; });
    }
    {   // какие стили — даты
        std::map<int, bool> custom;
        bool inXfs = false;
        walk(z.read("xl/styles.xml"), never,
            [&](const std::string& n, const std::string& a) {
                if (n == "numFmt") custom[std::atoi(attr(a, "numFmtId").c_str())] = codeIsDate(attr(a, "formatCode"));
                else if (n == "cellXfs") inXfs = true;
                else if (n == "xf" && inXfs) dateXf.push_back(isDateFmt(std::atoi(attr(a, "numFmtId").c_str()), custom));
            },
            [&](const std::string& n) { if (n == "cellXfs") inXfs = false; },
            [&](const std::string&) {});
    }
    walk(z.read("xl/workbook.xml"), never,
        [&](const std::string& n, const std::string& a) { if (n == "sheet") names.push_back(attr(a, "name")); },
        [&](const std::string&) {}, [&](const std::string&) {});

    std::string out;
    const auto parts = z.list("xl/worksheets/sheet", ".xml");
    for (size_t si = 0; si < parts.size() && out.size() < cap; ++si) {
        out += "## " + (si < names.size() ? names[si] : "Sheet" + std::to_string(si + 1)) + "\n";
        bool stop = false, inV = false;
        std::string type, val, line;
        int style = -1, nextCol = 0, tabs = 0, cellIdx = 0;
        walk(z.read(parts[si]), stop,
            [&](const std::string& n, const std::string& a) {
                if (n == "row") { line.clear(); nextCol = 0; tabs = 0; }
                else if (n == "c") {
                    std::string ref = attr(a, "r"), s = attr(a, "s");
                    type = attr(a, "t");
                    style = s.empty() ? -1 : std::atoi(s.c_str());
                    int ci = colIndex(ref);
                    cellIdx = ci >= 0 ? ci : nextCol;
                    val.clear();
                } else if (n == "v" || n == "t") inV = true;
            },
            [&](const std::string& n) {
                if (n == "v" || n == "t") inV = false;
                else if (n == "c") {
                    std::string r;
                    if (type == "s") {
                        size_t k = (size_t)std::strtoull(val.c_str(), nullptr, 10);
                        r = k < sst.size() ? sst[k] : "";
                    } else if (type == "b") r = val == "1" ? "TRUE" : "FALSE";
                    else if (type == "str" || type == "inlineStr" || type == "e") r = val;
                    else if (style >= 0 && (size_t)style < dateXf.size() && dateXf[style] && !val.empty()) {
                        double d = std::atof(val.c_str());
                        r = (d >= 1 && d < 2958466) ? isoFromSerial((long long)d) : val;
                    } else r = val;
                    r = text::oneLine(r);
                    if (!text::trim(r).empty()) {
                        while (tabs < cellIdx) { line += '\t'; ++tabs; }
                        line += r;
                    }
                    nextCol = cellIdx + 1;
                } else if (n == "row") {
                    if (!text::trim(line).empty()) out += line + "\n";
                    if (out.size() >= cap) stop = true;
                }
            },
            [&](const std::string& t) { if (inV) val += t; });
    }
    return out;
}

std::string officeText(const std::string& path, size_t cap) {
    Zip z(path);
    if (!z.ok()) {
        std::cerr << "[DocReader] WARNING: Failed to open as ZIP (invalid/corrupted Office file): " << path << "\n";
        return "";
    }
    if (z.has("word/document.xml")) return docxText(z, cap);
    if (z.has("xl/workbook.xml"))   return xlsxText(z, cap);
    if (z.has("ppt/presentation.xml")) return pptxText(z, cap);
    if (z.has("content.xml") && z.has("mimetype")) return odfText(z, cap);
    
    std::cerr << "[DocReader] WARNING: ZIP opened, but no known Office XML structure found in: " << path << "\n";
    return "";
}

// ============================================================
// Текст / XML
// ============================================================
std::string readHead(const std::string& path, size_t n) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::string b(n, '\0');
    f.read(&b[0], (std::streamsize)n);
    b.resize((size_t)f.gcount());
    return b;
}

bool looksXml(const std::string& s) {
    size_t p = s.find_first_not_of(" \t\r\n");
    if (p == std::string::npos || s[p] != '<') return false;
    std::string head = text::lower(s.substr(p, 300));
    return head.find("<html") == std::string::npos && head.find("<!doctype html") == std::string::npos;
}

// "InvoiceNumber" → "Invoice Number"; чтобы регулярки Extract находили метки.
std::string humanize(const std::string& n) {
    std::string o;
    for (size_t i = 0; i < n.size(); ++i) {
        char c = n[i];
        if (c == '_' || c == '-') { o += ' '; continue; }
        if (i > 0 && std::isupper((unsigned char)c) && std::islower((unsigned char)n[i - 1])) o += ' ';
        o += c;
    }
    return o;
}

// Листовые элементы → "Имя: значение [валюта]" по строке.
std::string flattenXml(const std::string& xml, size_t cap) {
    struct Node { std::string name, text, cur; bool kids = false; };
    std::vector<Node> st;
    std::string out;
    bool stop = false;
    walk(xml, stop,
        [&](const std::string& n, const std::string& a) {
            if (!st.empty()) st.back().kids = true;
            Node nd;
            nd.name = n;
            nd.cur = attr(a, "currencyID");
            if (nd.cur.empty()) nd.cur = attr(a, "currency");
            st.push_back(std::move(nd));
        },
        [&](const std::string&) {
            if (st.empty()) return;
            Node nd = std::move(st.back());
            st.pop_back();
            std::string v = text::trim(nd.text);
            if (!nd.kids && !v.empty())
                out += humanize(nd.name) + ": " + text::oneLine(v) + (nd.cur.empty() ? "" : " " + nd.cur) + "\n";
            if (out.size() >= cap) stop = true;
        },
        [&](const std::string& t) { if (!st.empty()) st.back().text += t; });
    return out;
}

std::string plainText(const std::string& path, size_t cap) {
    std::string raw = readHead(path, cap * 2 + 16);
    std::error_code ec;
    const auto fsize = (size_t)std::filesystem::file_size(path, ec);
    std::string t = toUtf8(raw);
    if (looksXml(t)) {
        if (!ec && fsize > raw.size()) t = toUtf8(readHead(path, std::min<size_t>(fsize, 8u << 20)));  // у XML нужна «вся» разметка
        return flattenXml(t, cap);
    }
    return t;
}

} // namespace

// ============================================================
// Публичный API
// ============================================================
std::string readText(const std::string& path, size_t max_chars, int max_pages) {
    using safety::FileKind;
    std::string out;
    switch (safety::sniff(path)) {
        case FileKind::Pdf: {
            PdfReadEngine r;
            if (!r.open(path)) return "";
            return r.extractLeadingText(max_pages, max_chars);
        }
        case FileKind::Text:   out = plainText(path, max_chars); break;
        case FileKind::Office: out = officeText(path, max_chars); break;
        default: return "";   // Image / Unknown
    }
    return text::cut(out, max_chars);
}

std::string readBinary(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}} // namespace mary::docread