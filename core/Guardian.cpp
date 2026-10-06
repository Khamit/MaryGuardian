#include "Guardian.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <ctime>

Guardian::Guardian(const std::string& log_path, const std::string& in, const std::string& out)
    : log_path_(log_path), input_root_(in), output_root_(out) {
    auto parent = std::filesystem::path(log_path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    std::filesystem::create_directories(in);
    std::filesystem::create_directories(out);
    loadTail();
}

// Load the latest records from previous sessions so the UI log does not reset.
void Guardian::loadTail() {
    std::ifstream f(log_path_);
    std::string line;
    while (std::getline(f, line)) {
        try {
            auto j = nlohmann::json::parse(line);
            log_.push_back({j.value("ts", ""), j.value("action", ""),
                            j.value("allowed", false), j.value("reason", "")});
            if (log_.size() > MAX_LOG) log_.pop_front();
        } catch (...) { /* malformed line - skip */ }
    }
}

void Guardian::record(const std::string& action, bool allowed, const std::string& reason) {
    std::lock_guard<std::mutex> lk(mu_);
    LogEntry e{now(), action, allowed, reason};
    log_.push_back(e);
    if (log_.size() > MAX_LOG) log_.pop_front();
    persist(e);
}

Verdict Guardian::check(const std::string& action, const std::string& content) {
    Verdict v;
    if (mary::safety::isBlockedAction(action)) {
        v.allowed = false;
        v.reason  = "Blocked: action '" + action + "' is not in the allowed list.";
        record(action, false, v.reason);
        return v;
    }
    if (!content.empty() && mary::safety::pii::contains(content)) {
        v.pii_warning = true;
        v.reason = "Allowed with warning: potential personally identifiable information (PII) detected.";
    } else {
        v.reason = "Allowed: check passed.";
    }
    record(action, true, v.reason);
    return v;
}

Verdict Guardian::checkFile(const std::string& action, const std::string& path,
                            bool for_write, bool log_allowed) {
    namespace fs = std::filesystem;
    Verdict v;
    const std::string label = action + ":" + fs::path(path).filename().string();
    auto deny = [&](const std::string& why) {
        v.allowed = false; v.reason = "Blocked: " + why;
        record(label, false, v.reason);
        return v;
    };
    if (!mary::safety::isAllowedAction(action)) return deny("action '" + action + "' is not allowed");
    if (!mary::safety::isInsideRoot(path, for_write ? output_root_ : input_root_))
        return deny("path is outside the allowed directory");
    if (!for_write) {
        std::error_code ec;
        if (!fs::exists(path, ec))
            return deny("file not found in the inbox directory: " + fs::path(path).filename().string());
        if (fs::is_symlink(path, ec) || !fs::is_regular_file(path, ec))
            return deny("not a regular file (directory or symbolic link)");
        if (fs::file_size(path, ec) > MAX_FILE_BYTES)
            return deny("file is too large");
        if (mary::safety::sniff(path) == mary::safety::FileKind::Unknown)
            return deny("unsupported format (only PDF and text are allowed)");
    }
    v.reason = "Allowed: file passed the check.";
    if (log_allowed) record(label, true, v.reason);
    return v;
}

Verdict Guardian::checkOutput(const std::string& text) {
    Verdict v;
    if (mary::safety::pii::contains(text)) {
        v.pii_warning = true;
        v.reason = "Allowed with warning: potential personally identifiable information (PII) detected in the output.";
    } else {
        v.reason = "Allowed: output checked.";
    }
    record("show_output", true, v.reason);
    return v;
}

std::deque<LogEntry> Guardian::entries() const {
    std::lock_guard<std::mutex> lk(mu_);
    return log_;
}

nlohmann::json Guardian::logJson() const {
    std::lock_guard<std::mutex> lk(mu_);
    auto j = nlohmann::json::array();
    for (const auto& e : log_) {
        j.push_back({{"ts", e.ts}, {"action", e.action},
                     {"allowed", e.allowed}, {"reason", e.reason}});
    }
    return j;
}

void Guardian::persist(const LogEntry& e) {
    std::ofstream f(log_path_, std::ios::app);
    if (!f.is_open()) return;
    nlohmann::json j{{"ts", e.ts}, {"action", e.action},
                     {"allowed", e.allowed}, {"reason", e.reason}};
    f << j.dump() << "\n";
}

std::string Guardian::now() const {
    auto tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_r(&tt, &tm);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}