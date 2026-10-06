#pragma once
#include <string>
#include <set>
#include <vector>
#include <regex>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cctype>

namespace mary { namespace safety {

inline std::string normalize(std::string s) {
    auto ws = [](unsigned char c){ return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), ws));
    s.erase(std::find_if_not(s.rbegin(), s.rend(), ws).base(), s.end());
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

// Default-deny: разрешено только то, что перечислено. Новое действие = осознанное решение.
inline const std::set<std::string>& allowedActions() {
    static const std::set<std::string> a = {
        "read", "draft", "export", "show_output",
        "index",      // сканирование inbox и классификация
        "organize"    // правки пользователя в дереве коллекций
    };
    return a;
}
inline bool isAllowedAction(const std::string& a) { return allowedActions().count(normalize(a)) > 0; }
inline bool isBlockedAction(const std::string& a) { return !isAllowedAction(a); } // совместимость

// Путь строго внутри корня (с учётом symlink и "..")
inline bool isInsideRoot(const std::filesystem::path& p, const std::filesystem::path& root) {
    std::error_code ec;
    auto rp = std::filesystem::weakly_canonical(p, ec);    if (ec) return false;
    auto rr = std::filesystem::weakly_canonical(root, ec); if (ec) return false;
    auto rel = rp.lexically_relative(rr);
    return !rel.empty() && *rel.begin() != ".." && *rel.begin() != ".";
}

// Формат определяем по содержимому, а не по расширению
enum class FileKind { Unknown, Pdf, Text, Office };

inline FileKind sniff(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return FileKind::Unknown;
    char b[4096]; 
    f.read(b, sizeof b);
    std::streamsize n = f.gcount();
    
    const unsigned char* ub = reinterpret_cast<const unsigned char*>(b);

    // 1. PDF
    if (n >= 5 && std::string(b, 5) == "%PDF-") return FileKind::Pdf;
    
    // 2. Office (ZIP-based: xlsx, docx, pptx, odt, ods)
    if (n >= 4 && ub[0] == 0x50 && ub[1] == 0x4B && 
       (ub[2] == 0x03 || ub[2] == 0x05 || ub[2] == 0x07) && ub[3] == 0x04) {
        return FileKind::Office;
    }

    // 3. Legacy Office (OLE2: старый бинарный .xls, .doc, .ppt)
    if (n >= 4 && ub[0] == 0xD0 && ub[1] == 0xCF && ub[2] == 0x11 && ub[3] == 0xE0) {
        return FileKind::Unknown; // Не поддерживается без libxls, но мы не будем парсить это как текст
    }
    
    if (n == 0) return FileKind::Unknown;
    
    // 4. Бинарные файлы (содержат нулевые байты)
    for (std::streamsize i = 0; i < n; ++i) {
        if (b[i] == '\0') return FileKind::Unknown;
    }
    
    return FileKind::Text;
}

namespace pii {
inline bool luhn(const std::string& digits) {
    int sum = 0; bool dbl = false;
    for (int i = (int)digits.size() - 1; i >= 0; --i) {
        int d = digits[i] - '0';
        if (dbl) { d *= 2; if (d > 9) d -= 9; }
        sum += d; dbl = !dbl;
    }
    return sum % 10 == 0;
}

inline bool containsCard(const std::string& text) {
    static const std::regex re(R"(\b(?:\d[ -]?){13,19}\b)");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re);
         it != std::sregex_iterator(); ++it) {
        std::string d;
        for (char c : it->str()) if (std::isdigit((unsigned char)c)) d += c;
        if (d.size() >= 13 && d.size() <= 19 && luhn(d)) return true;
    }
    return false;
}

inline const std::vector<std::regex>& patterns() {
    static const std::vector<std::regex> p = {
        std::regex(R"(\b\d{3}-\d{2}-\d{4}\b)"),                              // US SSN
        std::regex(R"(\bEIN\b[^\n]{0,10}\d{2}-\d{7}\b)"),                    // US EIN (только с ключевым словом)
        std::regex(R"(\b[A-Z]{2}\d{2}[A-Z0-9]{11,30}\b)"),                   // IBAN
        std::regex(R"(\b\d{3}-\d{3}-\d{3}[ -]\d{2}\b)"),                     // СНИЛС
        // ИНН/паспорт только с ключевым словом. Кириллица: icase не работает, регистры вручную.
        std::regex(R"((ИНН|инн|Инн)\s*:?\s*(\d{10}|\d{12})\b)"),
        std::regex(R"((паспорт|Паспорт|ПАСПОРТ)[^\n]{0,20}\d{4}\s?\d{6})"),
        std::regex(R"(\b\d{20}\b)"),                                         // р/с, к/с (20 цифр)
        std::regex(R"(\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b)"), // email
        std::regex(R"(\brouting\s*number\b|\baccount\s*number\b|\bssn\b|\bsocial\s*security\b)",
                   std::regex::icase)
    };
    return p;
}

inline bool contains(const std::string& text) {
    if (containsCard(text)) return true;
    for (const auto& re : patterns())
        if (std::regex_search(text, re)) return true;
    return false;
}
} // namespace pii
}} // namespace mary::safety