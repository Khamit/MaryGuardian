#pragma once
#include <string>
#include <vector>

namespace mary {

// Verifier — проверяет документы перед подачей в LLM.
// Защита от prompt injection (эвристика, не гарантия).
// PII-детекция живёт в Guardian через Safety::pii::contains().

class Verifier {
public:
    // Имена сработавших правил (пусто = ничего не найдено)
    std::vector<std::string> scan(const std::string& content);

    // Совместимость: true, если сработало хотя бы одно правило
    bool checkPromptInjection(const std::string& content);
};

} // namespace mary