#include "Extract.hpp"
#include <regex>
#include <cstdio>
#include <cctype>
#include <algorithm>
#include <set>

namespace mary { namespace extract {
namespace {

std::string low(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}
std::string trimStr(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}
bool validYMD(int y, int m, int d) {
    if (y < 1990 || y > 2100 || m < 1 || m > 12 || d < 1) return false;
    static const int dm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int mx = dm[m - 1];
    if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) mx = 29;
    return d <= mx;
}
std::string iso(int y, int m, int d) {
    char b[16]; std::snprintf(b, sizeof b, "%04d-%02d-%02d", y, m, d); return b;
}
int monthOf(const std::string& n) {
    static const char* m[] = {"jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec"};
    std::string p = low(n).substr(0, 3);
    for (int i = 0; i < 12; ++i) if (p == m[i]) return i + 1;
    return 0;
}
long long daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}
void civilFromDays(long long z, int& y, int& m, int& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int)yoe + (int)era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) ++y;
}
long long toMinor(std::string num) {
    num.erase(std::remove(num.begin(), num.end(), ','), num.end());
    auto dot = num.find('.');
    std::string whole = dot == std::string::npos ? num : num.substr(0, dot);
    std::string frac  = dot == std::string::npos ? "" : num.substr(dot + 1);
    if (whole.empty() || whole.size() > 12) return -1;
    while (frac.size() < 2) frac += '0';
    frac.resize(2);
    return std::stoll(whole) * 100 + std::stoll(frac);
}

// Ищет метку (группа 1) и дату в 40 символах после неё.
bool labelled(const std::string& t, const std::regex& re, std::string& date, std::string& label) {
    for (auto it = std::sregex_iterator(t.begin(), t.end(), re); it != std::sregex_iterator(); ++it) {
        size_t end = (size_t)it->position() + (size_t)it->length();
        std::string d = parseDate(t.substr(end, 40));
        if (!d.empty()) { date = d; label = low((*it)[1].str()); return true; }
    }
    return false;
}

} // namespace

std::string parseDate(const std::string& s) {
    static const std::string mon = "(jan|feb|mar|apr|may|jun|jul|aug|sep|oct|nov|dec)[a-z]*";
    static const std::regex isoRe(R"((\d{4})-(\d{1,2})-(\d{1,2}))");
    static const std::regex dotRe(R"((\d{1,2})\.(\d{1,2})\.(\d{4}))");
    static const std::regex slRe(R"((\d{1,2})/(\d{1,2})/(\d{4}))");
    static const std::regex dmyRe("(\\d{1,2})(?:st|nd|rd|th)?\\s+" + mon + "\\.?,?\\s+(\\d{4})", std::regex::icase);
    static const std::regex mdyRe(mon + "\\.?\\s+(\\d{1,2})(?:st|nd|rd|th)?,?\\s+(\\d{4})", std::regex::icase);

    size_t bestPos = std::string::npos;
    std::string best;
    auto consider = [&](const std::smatch& m, int y, int mo, int d) {
        if (validYMD(y, mo, d) && (size_t)m.position() < bestPos) { bestPos = (size_t)m.position(); best = iso(y, mo, d); }
    };
    std::smatch m;
    if (std::regex_search(s, m, isoRe)) consider(m, std::stoi(m[1]), std::stoi(m[2]), std::stoi(m[3]));
    if (std::regex_search(s, m, dotRe)) consider(m, std::stoi(m[3]), std::stoi(m[2]), std::stoi(m[1]));      // D.M.Y
    if (std::regex_search(s, m, slRe)) {
        int a = std::stoi(m[1]), b = std::stoi(m[2]), y = std::stoi(m[3]);
        if (a > 12) consider(m, y, b, a); else consider(m, y, a, b);                                            // M/D/Y (US)
    }
    if (std::regex_search(s, m, dmyRe)) consider(m, std::stoi(m[3]), monthOf(m[2]), std::stoi(m[1]));
    if (std::regex_search(s, m, mdyRe)) consider(m, std::stoi(m[3]), monthOf(m[1]), std::stoi(m[2]));
    return best;
}

std::string addDays(const std::string& isoDate, int days) {
    if (isoDate.size() != 10) return "";
    int y = std::stoi(isoDate.substr(0, 4)), m = std::stoi(isoDate.substr(5, 2)), d = std::stoi(isoDate.substr(8, 2));
    if (!validYMD(y, m, d)) return "";
    civilFromDays(daysFromCivil(y, m, d) + days, y, m, d);
    return iso(y, m, d);
}

std::string normalizeCompany(const std::string& name) {
    static const std::set<std::string> drop = {"inc","llc","ltd","corp","corporation","co","company",
                                               "gmbh","plc","limited","llp","lp","sa","ag","bv"};
    std::string s = low(name);
    for (char& c : s) if (!std::isalnum((unsigned char)c) && (unsigned char)c < 128) c = ' ';
    std::string out, tok;
    auto flush = [&] {
        if (!tok.empty() && !drop.count(tok)) { if (!out.empty()) out += ' '; out += tok; }
        tok.clear();
    };
    for (char c : s) { if (c == ' ') flush(); else tok += c; }
    flush();
    return out;
}

std::string formatMinor(long long minor) {
    if (minor < 0) return "";
    std::string whole = std::to_string(minor / 100), out;
    int cnt = 0;
    for (int i = (int)whole.size() - 1; i >= 0; --i) {
        out.insert(out.begin(), whole[i]);
        if (++cnt % 3 == 0 && i > 0) out.insert(out.begin(), ',');
    }
    char b[8]; std::snprintf(b, sizeof b, ".%02lld", minor % 100);
    return out + b;
}

Fields fromName(const std::string& filename) {
    Fields f;
    std::string n = low(filename);
    std::string d = n; std::replace(d.begin(), d.end(), '_', '-');
    f.date = parseDate(d);
    struct R { const char* type; std::regex re; };
    static const std::vector<R> rules = {
        {"credit_note", std::regex(R"((^|[^a-z])(credit[ _-]?note|credit[ _-]?memo|refund)([^a-z]|$))")},
        {"invoice",   std::regex(R"((^|[^a-z])(inv|invoice|bill)([^a-z]|$))")},
        {"receipt",   std::regex(R"((^|[^a-z])(rcpt|receipt)([^a-z]|$))")},
        {"contract",  std::regex(R"((^|[^a-z])(contract|agreement|nda)([^a-z]|$))")},
        {"statement", std::regex(R"((^|[^a-z])(stmt|statement)([^a-z]|$))")},
        {"tax",       std::regex(R"((^|[^a-z])(tax|1099|w-?2|vat)([^a-z]|$))")},
        {"letter",    std::regex(R"((^|[^a-z])(letter)([^a-z]|$))")},
        {"purchase_order", std::regex(R"((^|[^a-z])(purchase[ _-]?order|po[ _-]?[0-9]+)([^a-z]|$))")},
        {"quote",       std::regex(R"((^|[^a-z])(quote|quotation|estimate)([^a-z]|$))")},
        {"payslip",     std::regex(R"((^|[^a-z])(payslip|pay[ _-]?slip|pay[ _-]?stub|payroll|salary)([^a-z]|$))")},
        {"insurance",   std::regex(R"((^|[^a-z])(insurance|insurer)([^a-z]|$))")},
        {"utility",     std::regex(R"((^|[^a-z])(utility|utilities|electricity|internet|telecom)([^a-z]|$))")},
        {"legal",       std::regex(R"((^|[^a-z])(court|lawsuit|subpoena|affidavit|summons|power[ _-]?of[ _-]?attorney)([^a-z]|$))")},
        {"identity",    std::regex(R"((^|[^a-z])(passport|driver[ _-]?licen[sc]e|id[ _-]?card|identity)([^a-z]|$))")},
        {"medical",     std::regex(R"((^|[^a-z])(medical|prescription|diagnosis)([^a-z]|$))")},
        {"certificate", std::regex(R"((^|[^a-z])(certificate|diploma)([^a-z]|$))")},
        {"report",      std::regex(R"((^|[^a-z])(report|minutes)([^a-z]|$))")},
    };
    for (const auto& r : rules) if (std::regex_search(n, r.re)) { f.type = r.type; break; }
    return f;
}

Fields fromText(const std::string& text) {
    Fields f;
    const std::string t = text.size() > 6000 ? text.substr(0, 6000) : text;
    const std::string head = low(t.substr(0, 800));
    auto has = [&](const char* w) { return head.find(w) != std::string::npos; };

    // --- тип ---
    if (has("credit note") || has("credit memo")) f.type = "credit_note";
    else if (has("invoice")) f.type = "invoice";
    else if (has("receipt")) f.type = "receipt";
    else if (has("bank statement") || has("account statement") || has("statement of account")) f.type = "statement";
    else if (has("agreement") || has("contract")) f.type = "contract";
    else if (has("tax return") || has("form 1099") || has("form w-2") || has("vat return")) f.type = "tax";
    else if (has("payslip") || has("pay slip") || has("pay stub") || has("gross pay") || has("net pay")) f.type = "payslip";
    else if (has("insurance policy") || has("policy number") || has("certificate of insurance") || has("insurance")) f.type = "insurance";
    else if (has("purchase order")) f.type = "purchase_order";
    else if (has("quotation") || has("price quote") || has("quote no") || has("estimate no")) f.type = "quote";
    else if (has("kwh") || has("meter reading") || has("electricity") || has("utility")) f.type = "utility";
    else if (has("plaintiff") || has("defendant") || has("power of attorney") || has("affidavit") || has("summons") || has("district court") || has("superior court")) f.type = "legal";
    else if (has("passport") || has("driver license") || has("driver's license") || has("identity card")) f.type = "identity";
    else if (has("patient") || has("prescription") || has("diagnosis") || has("medical")) f.type = "medical";
    else if (has("certificate of") || has("this is to certify") || has("diploma")) f.type = "certificate";
    else if (has("annual report") || has("meeting minutes") || has("minutes of the")) f.type = "report";
    else if (has("dear ") && (low(t).find("sincerely") != std::string::npos || low(t).find("regards") != std::string::npos)) f.type = "letter";

    // --- дата документа (метка "date", но не "due date" / "valid until") ---
    {
        static const std::regex lab(R"(\b(invoice\s+date|date\s+of\s+issue|issue\s+date|receipt\s+date|statement\s+date|issued|dated|date)\b\s*[:#]?)", std::regex::icase);
        for (auto it = std::sregex_iterator(t.begin(), t.end(), lab); it != std::sregex_iterator(); ++it) {
            size_t pos = (size_t)it->position(), end = pos + (size_t)it->length();
            std::string label = low((*it)[1].str());
            if (label == "date" || label == "dated" || label == "issued") {
                size_t ls = t.rfind('\n', pos);
                ls = (ls == std::string::npos) ? 0 : ls + 1;
                std::string pre = low(t.substr(ls, pos - ls));
                if (pre.find("due") != std::string::npos || pre.find("valid") != std::string::npos ||
                    pre.find("expir") != std::string::npos || pre.find("end") != std::string::npos ||
                    pre.find("term") != std::string::npos || pre.find("payment") != std::string::npos) continue;
            }
            std::string d = parseDate(t.substr(end, 40));
            if (!d.empty()) { f.date = d; break; }
        }
        if (f.date.empty()) f.date = parseDate(t.substr(0, 800));
    }

    // --- номер ---
    {
        static const std::regex re(R"((?:invoice|inv|receipt|order|reference|ref|contract|statement)\s*(?:no\.?|number|num\.?|#)\s*[:#]?\s*([A-Za-z0-9][A-Za-z0-9\-/]{2,24}))", std::regex::icase);
        for (auto it = std::sregex_iterator(t.begin(), t.end(), re); it != std::sregex_iterator(); ++it) {
            std::string n = (*it)[1].str();
            if (std::any_of(n.begin(), n.end(), [](unsigned char c) { return std::isdigit(c); })) { f.number = n; break; }
        }
        if (f.number.empty()) {
            static const std::regex re2(R"(\b((?:INV|RCPT|REC)[-_]?\d{3,}[A-Za-z0-9\-]*)\b)", std::regex::icase);
            std::smatch m;
            if (std::regex_search(t, m, re2)) f.number = m[1].str();
        }
    }

    // --- сумма и валюта ---
    {
        static const char* labels[] = {"amount\\s+due", "total\\s+due", "balance\\s+due", "grand\\s+total", "total\\s+amount", "total"};
        static const std::vector<std::regex> res = [] {
            std::vector<std::regex> v;
            for (const char* l : labels)
                v.emplace_back(std::string("\\b(?:") + l + ")\\b[^\\n0-9$\xE2\x82\xAC\xC2\xA3]{0,25}"
                               "(\\$|\xE2\x82\xAC|\xC2\xA3|USD|EUR|GBP)?\\s*"
                               "([0-9]{1,3}(?:,[0-9]{3})+(?:\\.[0-9]{1,2})?|[0-9]+(?:\\.[0-9]{1,2})?)\\s*(USD|EUR|GBP)?",
                               std::regex::icase);
            return v;
        }();
        for (const auto& re : res) {
            std::smatch m;
            if (!std::regex_search(t, m, re)) continue;
            long long minor = toMinor(m[2].str());
            if (minor < 0) continue;
            f.amount_minor = minor;
            std::string cur = m[1].matched ? m[1].str() : (m[3].matched ? m[3].str() : "");
            if (cur == "$") cur = "USD"; else if (cur == "\xE2\x82\xAC") cur = "EUR"; else if (cur == "\xC2\xA3") cur = "GBP";
            f.currency = cur.empty() ? "" : [&] { std::string u = cur; for (char& c : u) c = (char)std::toupper((unsigned char)c); return u; }();
            break;
        }
        if (f.amount_minor >= 0 && f.currency.empty()) {
            if (t.find('$') != std::string::npos) f.currency = "USD";
            else if (t.find("\xE2\x82\xAC") != std::string::npos) f.currency = "EUR";
            else if (t.find("\xC2\xA3") != std::string::npos) f.currency = "GBP";
        }
    }

    // --- компания-отправитель ---
    {
        static const std::regex re(R"((?:from|vendor|supplier|seller|billed\s+by|issued\s+by|merchant)\s*:\s*([^\n]{2,60}))", std::regex::icase);
        std::smatch m;
        if (std::regex_search(t, m, re)) {
            std::string c = m[1].str();
            size_t gap = c.find("  "); if (gap != std::string::npos) c.resize(gap);
            size_t tab = c.find('\t'); if (tab != std::string::npos) c.resize(tab);
            c = trimStr(c);
            while (!c.empty() && (c.back() == '.' || c.back() == ',' || c.back() == ';')) c.pop_back();
            bool alldig = std::all_of(c.begin(), c.end(), [](unsigned char x) { return std::isdigit(x) || x == ' '; });
            if (c.size() >= 2 && !alldig) f.company = c;
        }
    }

    // --- срок: expires > term > due > net ---
    {
        static const std::regex exp(R"(\b((?:valid\s+(?:until|through|thru|to)|expires?(?:\s+on)?|expiry(?:\s+date)?|expiration(?:\s+date)?|good\s+until))\s*[:\-]?)", std::regex::icase);
        static const std::regex term(R"(\b((?:end\s+date|termination\s+date|term\s+ends?(?:\s+on)?|renewal\s+date))\s*[:\-]?)", std::regex::icase);
        static const std::regex due(R"(\b((?:due\s+date|payment\s+due|due\s+on|due\s+by))\s*[:\-]?)", std::regex::icase);
        std::string d, l;
        if (labelled(t, exp, d, l))       { f.expires = d; f.expires_basis = "expires"; }
        else if (labelled(t, term, d, l)) { f.expires = d; f.expires_basis = "term"; }
        else if (labelled(t, due, d, l))  { f.expires = d; f.expires_basis = "due"; }
        else if (!f.date.empty()) {
            static const std::regex net(R"((?:\bnet\s*(\d{1,3})\b|payment\s+terms?\s*[:\-]?\s*(\d{1,3})\s*days))", std::regex::icase);
            std::smatch m;
            if (std::regex_search(t, m, net)) {
                int days = std::stoi(m[1].matched ? m[1].str() : m[2].str());
                std::string e = addDays(f.date, days);
                if (!e.empty()) { f.expires = e; f.expires_basis = "net"; }
            }
        }
    }
    return f;
}

}} // namespace mary::extract