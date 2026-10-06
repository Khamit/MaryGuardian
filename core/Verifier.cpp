#include "Verifier.hpp"
#include <regex>

namespace mary {

static std::string stripInvisible(std::string s) {
    for (const char* seq : {"\xE2\x80\x8B", "\xE2\x80\x8C", "\xE2\x80\x8D", "\xE2\x81\xA0", "\xEF\xBB\xBF"}) {
        size_t p;
        while ((p = s.find(seq)) != std::string::npos) s.erase(p, 3);
    }
    return s;
}

std::vector<std::string> Verifier::scan(const std::string& raw) {
    static const std::vector<std::pair<std::string, std::regex>> rules = {
        {"ignore_previous_en", std::regex(R"((ignore|disregard)\s+(all\s+)?(previous|prior)\s+(instructions|prompts))", std::regex::icase)},
        {"system_marker",      std::regex(R"(system\s*(notice|override|prompt)\s*:)", std::regex::icase)},
        {"exfiltrate_en",      std::regex(R"((upload|send|transmit)\s+(all|full)\s+(data|ssn|documents))", std::regex::icase)},
        {"hide_from_user_en",  std::regex(R"(do\s*not\s*(tell|reveal)\s+(user|human))", std::regex::icase)},
        // Кириллица: регистры перечисляем вручную
        {"ignore_previous_ru", std::regex(R"((игнорируй|Игнорируй|ИГНОРИРУЙ|забудь|Забудь)[^\n]{0,80}(инструкци|указани))")},
        {"hide_from_user_ru",  std::regex(R"((не\s+сообщай|не\s+говори|не\s+показывай)[^\n]{0,20}(пользовател))")},
        {"chatml_tokens",      std::regex(R"(<\|im_(start|end)\|>)")},
    };
    const std::string s = stripInvisible(raw);
    std::vector<std::string> hits;
    for (const auto& [name, re] : rules) if (std::regex_search(s, re)) hits.push_back(name);
    return hits;
}

} // namespace mary