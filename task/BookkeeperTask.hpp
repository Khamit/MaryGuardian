#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "../core/Guardian.hpp"
#include "../core/Verifier.hpp"
#include "../core/ModelRuntime.hpp"

namespace mary {

// BookkeeperTask — 3 шага:
//
//   extract(paths)           — читает PDF/TXT → текст
//   draft(instruction)       — LLM генерирует черновик, Guardian проверяет
//   [пользователь смотрит]   — Guardian ждёт approve/reject
//   exportTo(path)           — сохраняет утверждённый черновик
//
// Заменяет: DisputeTask (7 фаз), StateEngine, AgentGraph, ContextAssembler,
//           KnowledgeBase, DocumentStore, UserProfile, ModelRouter.

enum class Step { IDLE, EXTRACTED, DRAFT_READY, APPROVED, ERROR };

struct TaskState {
    Step        step        = Step::IDLE;
    std::string extracted;      // объединённый текст всех документов
    std::string draft;          // черновик от LLM
    bool        pii_warning = false;
    bool truncated = false;
    std::string error;
};

class BookkeeperTask {
public:
    BookkeeperTask(ModelRuntime& model, Guardian& guardian, Verifier& verifier);

    // --- Шаг 1: Извлечь текст ---
    // Принимает пути к PDF и/или TXT. Проверяет prompt injection.
    // Возвращает false если файлы нечитаемы или injection обнаружен.
    bool extract(const std::vector<std::string>& paths);

    // --- Шаг 2: Сгенерировать черновик ---
    // instruction — произвольная задача ("составь письмо-претензию",
    //               "суммируй начисления за март", "найди расхождения")
    // Guardian проверяет вывод на PII и ставит флаг pending_approval.
    bool draft(const std::string& instruction = "");

    // --- Шаг 3: Утвердить (или отредактировать) ---
    void approve();
    void editAndApprove(const std::string& edited_text);

    // --- Экспорт ---
    bool exportTo(const std::string& output_path) const;

    // --- Состояние ---
    const TaskState& state() const { return state_; }
    void reset();

private:
    std::string readFile(const std::string& path) const;
    std::string extractPdfText(const std::string& path) const;
    std::string buildPrompt(const std::string& instruction, const std::string& doc) const;

    ModelRuntime& model_;
    Guardian&     guardian_;
    Verifier&     verifier_;
    TaskState     state_;
};

} // namespace mary