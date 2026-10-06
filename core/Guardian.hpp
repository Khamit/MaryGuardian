#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <deque>
#include <nlohmann/json.hpp>
#include "Safety.hpp"

// Guardian — единственный охранный слой между LLM и пользователем.
//   1. Белый список действий (default-deny)
//   2. PII-детекция перед показом
//   3. Human-in-the-loop (approve/reject черновик)
//   4. Лог всех действий в JSONL-файл

struct Verdict {
    bool allowed     = true;
    bool pii_warning = false;
    std::string reason;
};

struct LogEntry {
    std::string ts;
    std::string action;
    bool allowed;
    std::string reason;
};

class Guardian {
public:
    Guardian(const std::string& log_path, const std::string& input_root, const std::string& output_root);

    Verdict check(const std::string& action, const std::string& content = "");
    Verdict checkOutput(const std::string& text);
    // log_allowed=false: не писать успешные проверки в журнал (массовый скан inbox).
    // Отказы пишутся всегда.
    Verdict checkFile(const std::string& action, const std::string& path,
                      bool for_write = false, bool log_allowed = true);
    void    record(const std::string& action, bool allowed, const std::string& reason);

    // --- Human-in-the-loop ---
    void pendingApproval(bool v) { pending_ = v; approved_ = false; }
    bool isPending()  const { return pending_; }
    void approve()          { approved_ = true;  pending_ = false; }
    void reject()           { approved_ = false; pending_ = false; }
    bool isApproved() const { return approved_; }

    // --- Журнал аудита ---
    std::deque<LogEntry> entries() const;
    nlohmann::json logJson() const;

private:
    void persist(const LogEntry& e);
    void loadTail();
    std::string now() const;

    std::string          log_path_;
    std::deque<LogEntry> log_;
    std::atomic<bool>    pending_{false};
    std::atomic<bool>    approved_{false};

    static constexpr size_t MAX_LOG = 500;
    mutable std::mutex mu_;
    std::string input_root_, output_root_;
    static constexpr size_t MAX_FILE_BYTES = 50u * 1024 * 1024;
};