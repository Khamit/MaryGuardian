#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include "../core/Guardian.hpp"
#include "../core/ModelRuntime.hpp"
#include "../task/BookkeeperTask.hpp"
#include "../core/Indexer.hpp"
#include "../core/Library.hpp"

// HttpServer — единственная реализация HTTP (только loopback, токен, проверка Host/Origin).
//   GET  /              → index.html
//   GET  /api/status    → шаг, модель загружена
//   POST /api/upload    → {paths:[...]}  → извлечь текст
//   POST /api/draft     → {instruction}  → черновик
//   POST /api/approve   → утвердить
//   POST /api/edit      → {draft}        → править и утвердить
//   POST /api/export    → {name}         → сохранить файл
//   GET  /api/audit     → журнал аудита

class HttpServer {
public:
    HttpServer(int port,
               mary::BookkeeperTask& task,
               Guardian& guardian,
               mary::ModelRuntime& model,
               mary::Indexer& indexer,
               mary::Library& library,
               const std::string& workspace = ".");
    ~HttpServer() { stop(); }

    bool start();
    void stop();
    void setRunningFlag(std::atomic<bool>* flag) { running_flag_ = flag; }

private:
    void run();
    void handleClient(int client_fd);

    std::string route(const std::string& method,
                      const std::string& path,
                      const std::string& body);

    std::string apiStatus();
    std::string apiLib(const std::string& op, const nlohmann::json& body);
    std::string apiUpload(const nlohmann::json& body);
    std::string apiDraft(const nlohmann::json& body);
    std::string apiApprove();
    std::string apiEdit(const nlohmann::json& body);
    std::string apiExport(const nlohmann::json& body);
    std::string apiAudit();

    std::string loadHtml() const;
    static std::string jsonOk(const nlohmann::json& data = {});
    static std::string jsonError(const std::string& msg);
    static std::string httpResponse(int code,
                                    const std::string& content_type,
                                    const std::string& body);

    int                      port_;
    mary::BookkeeperTask&    task_;
    Guardian&                guardian_;
    mary::ModelRuntime&      model_;
    mary::Indexer&           indexer_;
    mary::Library&           library_;
    std::string              workspace_;

    int                      server_fd_ = -1;
    std::atomic<bool>        running_{false};
    std::atomic<bool>*       running_flag_ = nullptr;
    std::atomic<int>         active_{0};      // число живых обработчиков запросов
    std::thread              server_thread_;
    std::mutex               mutex_;
    std::string              token_;
};