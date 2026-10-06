#pragma once
#include <string>
#include <algorithm>
#include <cctype>

namespace mary { namespace text {

inline void utf8TrimTail(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1; int back = 0;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80 && back < 3) { --i; ++back; }
    unsigned char c = (unsigned char)s[i];
    size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (i + need > s.size()) s.resize(i);
}
inline std::string cut(std::string s, size_t n) {
    if (s.size() > n) { s.resize(n); utf8TrimTail(s); }
    return s;
}
inline std::string oneLine(std::string s) {
    for (char& c : s) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return s;
}
inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}
inline std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}
// Любой "<" перед буквой, "/", "|" или "!" получает пробел: документ не может
// создать special-токен или закрыть блок данных (<document>, <cards>, <|im_end|>, <think>).
inline std::string neutralize(const std::string& s) {
    std::string o; o.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size(); ++i) {
        o += s[i];
        if (s[i] == '<' && i + 1 < s.size()) {
            unsigned char n = (unsigned char)s[i + 1];
            if (std::isalpha(n) || n == '/' || n == '|' || n == '!') o += ' ';
        }
    }
    return o;
}
inline std::string stripThink(std::string s) {
    size_t a;
    while ((a = s.find("<think>")) != std::string::npos) {
        size_t b = s.find("</think>", a);
        if (b == std::string::npos) { s.erase(a); break; }
        s.erase(a, b + 8 - a);
    }
    size_t c = s.find("</think>");
    if (c != std::string::npos) s.erase(0, c + 8);
    return s;
}

}} // namespace mary::text