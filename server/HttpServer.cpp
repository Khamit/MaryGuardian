#include "HttpServer.hpp"
#include "ApiHandlers.hpp"
#include "core/NeuralFieldSystem.hpp"
#include "core/AgentAuditBridge.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>

HttpServer::HttpServer(int port, const std::string& workspace, NeuralFieldSystem* nfs, AgentAuditBridge* auditor)
    : port_(port), workspace_(workspace), server_fd_(-1), running_(false), nfs_(nfs), auditor_(auditor), running_flag_(nullptr) {
    apiHandlers_ = std::make_unique<ApiHandlers>(nfs, auditor, workspace);
}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start() {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        std::cerr << "[HTTP] Failed to create socket" << std::endl;
        return false;
    }
    
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);
    
    if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "[HTTP] Bind failed on port " << port_ << std::endl;
        return false;
    }
    
    if (listen(server_fd_, 10) < 0) {
        std::cerr << "[HTTP] Listen failed" << std::endl;
        return false;
    }
    
    running_ = true;
    server_thread_ = std::thread(&HttpServer::run, this);
    std::cout << "[HTTP] Server started on port " << port_ << std::endl;
    return true;
}

void HttpServer::stop() {
    if (!running_.load()) return;  // Уже остановлен
    
    running_ = false;
    
    // Принудительно закрываем серверный сокет, чтобы разблокировать accept()
    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }
    
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void HttpServer::run() {
    while (running_.load()) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (running_.load()) {
                std::cerr << "[HTTP] Accept failed" << std::endl;
            }
            continue;
        }
        
        std::thread(&HttpServer::handleClient, this, client_fd).detach();
    }
}

std::string HttpServer::loadHtmlFile(const std::string& path) {
    std::string file_path = "web/" + path;
    if (path == "/" || path == "/index.html") {
        file_path = "web/index.html";
    }
    
    std::ifstream html_file(file_path);
    if (html_file.is_open()) {
        std::stringstream buffer;
        buffer << html_file.rdbuf();
        return buffer.str();
    }
    
    std::cerr << "[HTTP] ERROR: Could not find " << file_path << std::endl;
    return "<html><body><h1>Error</h1><p>Page not found: " + path + "</p></body></html>";
}

void HttpServer::handleGetRequest(const std::string& path, std::string& response) {
    // HTML страницы
    if (path == "/" || path == "/index.html") {
        response = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n" + loadHtmlFile("index.html");
    }
    else if (path == "/agents.html") {
        response = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n" + loadHtmlFile("agents.html");
    }
    else if (path == "/auditor.html") {
        response = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n" + loadHtmlFile("auditor.html");
    }
    else if (path == "/analytics.html") {
        response = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n" + loadHtmlFile("analytics.html");
    }
    else if (path == "/css/style.css") {
        std::ifstream css_file("web/css/style.css");
        if (css_file.is_open()) {
            std::stringstream buffer;
            buffer << css_file.rdbuf();
            response = "HTTP/1.1 200 OK\r\nContent-Type: text/css\r\n\r\n" + buffer.str();
        } else {
            response = "HTTP/1.1 404 Not Found\r\n\r\n";
        }
    }
    // API эндпоинты
    else if (path == "/api/status") {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleStatus();
    }
    else if (path == "/api/constraints") {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleConstraints();
    }
    else if (path == "/api/auditor/status") {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleAuditorStatus();
    }
    else if (path == "/api/analytics") {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleAnalytics();
    }
    else if (path == "/api/agents") {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleAgentsList();
    }
    else if (path == "/api/demo/status") {
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleDemoStatus();
    }
    else if (path == "/api/last_action") {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleLastAction();
    }
    else if (path == "/api/metrics") {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleMetrics();
    }
    else if (path == "/api/selfsignals") {
        auto snap = nfs_->getSelfSignalSampler().last();
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + 
                nlohmann::json(snap.toVector()).dump();
    }
    else if (path == "/api/events") {
        response = "HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/event-stream\r\n"
                   "Cache-Control: no-cache\r\n"
                   "Connection: keep-alive\r\n"
                   "Access-Control-Allow-Origin: *\r\n\r\n";
        // SSE connection handled separately in handleClient
    }
    else if (path.find("/api/stats/") == 0) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        std::string period = path.substr(11);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleStats(period);
    }
    else {
        response = "HTTP/1.1 404 Not Found\r\n\r\n";
    }
}

void HttpServer::handlePostRequest(const std::string& path, const std::string& body, std::string& response) {
    nlohmann::json json_body;
    try {
        if (!body.empty()) {
            json_body = nlohmann::json::parse(body);
        }
    } catch (...) {
        response = "HTTP/1.1 400 Bad Request\r\n\r\n";
        return;
    }
    
    if (path == "/api/constraint") {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleConstraintToggle(json_body);
    }
    else if (path == "/api/session/start") {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleSessionStart(json_body);
    }
    else if (path == "/api/session/end") {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleSessionEnd();
    }
    else if (path == "/api/save") {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleSaveState();
    }
    else if (path == "/api/reset_risk") {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleResetRisk();
    }
    else if (path == "/api/auditor/toggle") {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleAuditorToggle(json_body);
    }
    else if (path == "/api/auditor/reset") {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleAuditorReset();
    }
    else if (path == "/api/auditor/config/save") {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleAuditorConfigSave(json_body);
    }
    else if (path == "/api/demo/start") {
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleDemoStart();
    }
    else if (path == "/api/demo/stop") {
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleDemoStop();
    }
    else if (path == "/api/demo/scenario") {
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleDemoScenario();
    }
    else if (path == "/api/agent/register") {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleRegisterAgent(json_body);
    }
    else if (path == "/api/agent/heartbeat") {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleHeartbeat(json_body);
    }
    else if (path == "/api/audit/action") {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + apiHandlers_->handleAuditAction(json_body);
    }
    else {
        response = "HTTP/1.1 404 Not Found\r\n\r\n";
    }
}

void HttpServer::handleClient(int client_fd) {
    char buffer[8192] = {0};
    int bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    
    if (bytes_read <= 0) {
        close(client_fd);
        return;
    }
    
    std::string request(buffer);
    std::string method, path, version;
    std::istringstream iss(request);
    iss >> method >> path >> version;
    
    // Извлекаем тело запроса для POST
    std::string body;
    size_t body_pos = request.find("\r\n\r\n");
    if (body_pos != std::string::npos) {
        body = request.substr(body_pos + 4);
    }
    
    std::string response;
    
    // Обработка SSE (Server-Sent Events) - ИСПРАВЛЕНО!
    if (path == "/api/events") {
        std::string sse_header = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/event-stream\r\n"
                                 "Cache-Control: no-cache\r\n"
                                 "Connection: keep-alive\r\n"
                                 "Access-Control-Allow-Origin: *\r\n\r\n";
        
        if (send(client_fd, sse_header.c_str(), sse_header.length(), 0) < 0) {
            close(client_fd);
            return;
        }
        
        // Отправляем события - используем атомарные проверки
        int event_counter = 0;
        while (running_.load()) {
            // Проверяем внешний флаг атомарно
            if (running_flag_ && !running_flag_->load()) {
                break;
            }
            
            // Блокируем мьютекс перед получением данных
            nlohmann::json event;
            {
                std::lock_guard<std::mutex> lock(data_mutex_);
                event = apiHandlers_->getEventData();
            }
            
            std::string data = "data: " + event.dump() + "\n\n";
            if (send(client_fd, data.c_str(), data.length(), 0) < 0) {
                // Клиент отключился
                break;
            }
            
            event_counter++;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        close(client_fd);
        return;
    }
    
    if (method == "GET") {
        handleGetRequest(path, response);
    } else if (method == "POST") {
        handlePostRequest(path, body, response);
    } else {
        response = "HTTP/1.1 405 Method Not Allowed\r\n\r\n";
    }
    
    send(client_fd, response.c_str(), response.length(), 0);
    close(client_fd);
}