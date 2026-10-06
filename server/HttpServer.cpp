#include <nlohmann/json.hpp>
#include "HttpServer.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <filesystem>
#include <unordered_map>
#include <algorithm>
#include <sys/time.h>
#include <cctype>
#include <poll.h>

namespace {
// Безопасные геттеры: неверный тип поля не должен ронять процесс.
std::string getStr(const nlohmann::json& j, const char* key, const std::string& def = "") {
    if (j.is_object() && j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return def;
}

void sendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = send(fd, s.data() + off, s.size() - off, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        off += (size_t)n;
    }
}
} // namespace

    HttpServer::HttpServer(int port,
                       mary::BookkeeperTask& task,
                       Guardian& guardian,
                       mary::ModelRuntime& model,
                       mary::Indexer& indexer,
                       mary::Library& library,
                       const std::string& workspace)
    : port_(port), task_(task), guardian_(guardian),
      model_(model), indexer_(indexer), library_(library), workspace_(workspace) {
    std::ifstream ur("/dev/urandom", std::ios::binary);
    unsigned char b[16] = {0};
    ur.read((char*)b, sizeof b);
    static const char* hex = "0123456789abcdef";
    for (unsigned char c : b) { token_ += hex[c >> 4]; token_ += hex[c & 15]; }
}

bool HttpServer::start() {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) { std::cerr << "[HTTP] socket failed\n"; return false; }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(port_);

    if (bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[HTTP] bind failed on port " << port_ << "\n";
        close(server_fd_); server_fd_ = -1;
        return false;
    }
    if (listen(server_fd_, 16) < 0) {
        std::cerr << "[HTTP] listen failed\n";
        close(server_fd_); server_fd_ = -1;
        return false;
    }

    running_ = true;
    server_thread_ = std::thread(&HttpServer::run, this);
    std::cout << "[HTTP] Listening on http://localhost:" << port_ << "\n";
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;
    if (server_thread_.joinable()) server_thread_.join();
    if (server_fd_ >= 0) { close(server_fd_); server_fd_ = -1; }
    // Ждём активные запросы (до 3 с). Долгая генерация дождётся model.unload() через gen_mu_.
    for (int i = 0; i < 60 && active_.load() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void HttpServer::run() {
    while (running_.load() && (!running_flag_ || running_flag_->load())) {
        pollfd pfd{server_fd_, POLLIN, 0};
        if (poll(&pfd, 1, 500) <= 0) continue;
        sockaddr_in client_addr{};
        socklen_t   len = sizeof(client_addr);
        int fd = accept(server_fd_, (sockaddr*)&client_addr, &len);
        if (fd < 0) continue;
        ++active_;                                   // до старта потока
        std::thread(&HttpServer::handleClient, this, fd).detach();
    }
}

static std::string headerValue(const std::string& headers, std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    std::istringstream is(headers); std::string line;
    while (std::getline(is, line)) {
        auto c = line.find(':'); if (c == std::string::npos) continue;
        std::string k = line.substr(0, c);
        std::transform(k.begin(), k.end(), k.begin(), ::tolower);
        if (k == name) {
            std::string v = line.substr(c + 1);
            v.erase(0, v.find_first_not_of(" \t"));
            while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
            return v;
        }
    }
    return "";
}

static bool readRequest(int fd, std::string& out) {
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    constexpr size_t MAX_REQ = 1 << 20;
    char buf[4096];
    size_t header_end = std::string::npos, content_len = 0;
    while (out.size() < MAX_REQ) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) return false;
        out.append(buf, (size_t)n);
        if (header_end == std::string::npos && (header_end = out.find("\r\n\r\n")) != std::string::npos) {
            header_end += 4;
            std::string cl = headerValue(out.substr(0, header_end), "content-length");
            content_len = cl.empty() ? 0 : (size_t)std::strtoull(cl.c_str(), nullptr, 10);
            if (content_len > MAX_REQ) return false;
        }
        if (header_end != std::string::npos && out.size() >= header_end + content_len) return true;
    }
    return false;
}

void HttpServer::handleClient(int fd) {
    struct Guard { std::atomic<int>& n; ~Guard() { --n; } } guard{active_};

    std::string request;
    if (!readRequest(fd, request)) { close(fd); return; }

    std::string response;
    try {
        auto hdr_end = request.find("\r\n\r\n");
        std::string headers = request.substr(0, hdr_end);
        std::string body    = request.substr(hdr_end + 4);

        std::istringstream iss(request);
        std::string method, raw_path, version;
        iss >> method >> raw_path >> version;
        std::string path = raw_path.substr(0, raw_path.find('?'));

        const std::string p = std::to_string(port_);
        const std::string host = headerValue(headers, "host");
        const bool host_ok = (host == "localhost:" + p || host == "127.0.0.1:" + p);
        const std::string origin = headerValue(headers, "origin");
        const bool origin_ok = origin.empty() || origin == "http://localhost:" + p || origin == "http://127.0.0.1:" + p;
        const bool is_api = path.rfind("/api/", 0) == 0;
        const bool token_ok = !is_api || headerValue(headers, "x-auth-token") == token_;

        if (!host_ok || !origin_ok || !token_ok) {
            guardian_.record("http_rejected:" + path, false, "Invalid Host/Origin/token");
            response = httpResponse(403, "text/plain", "Forbidden");
        } else {
            response = route(method, path, body);
        }
    } catch (const std::exception& e) {
        std::cerr << "[HTTP] handler error: " << e.what() << "\n";
        response = httpResponse(500, "application/json",
                                nlohmann::json{{"ok", false}, {"error", "internal error"}}.dump());
    } catch (...) {
        response = httpResponse(500, "text/plain", "Internal Server Error");
    }
    sendAll(fd, response);
    close(fd);
}

std::string HttpServer::route(const std::string& method,
                              const std::string& path,
                              const std::string& body) {
    if (method == "GET" && (path == "/" || path == "/index.html")) {
        std::string html = loadHtml();
        for (size_t p = html.find("__TOKEN__"); p != std::string::npos;
             p = html.find("__TOKEN__", p + token_.size()))
            html.replace(p, 9, token_);
        return httpResponse(200, "text/html; charset=utf-8", html);
    }

    if (method == "GET" && path.rfind("/static/", 0) == 0) {
        namespace fs = std::filesystem;
        fs::path root = fs::path(workspace_) / "web";
        fs::path file = root / path.substr(8);
        if (path.find("..") != std::string::npos ||
            !mary::safety::isInsideRoot(file, root) || !fs::is_regular_file(file))
            return httpResponse(404, "text/plain", "Not Found");
        std::ifstream f(file, std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        std::string ext = file.extension().string();
        std::string ct = ext == ".css"   ? "text/css"
                       : ext == ".js"    ? "text/javascript"
                       : ext == ".svg"   ? "image/svg+xml"
                       : ext == ".woff2" ? "font/woff2"
                       : ext == ".woff"  ? "font/woff"
                                         : "application/octet-stream";
        return httpResponse(200, ct, ss.str());
    }

    auto parseBody = [&]() -> nlohmann::json {
        try { return body.empty() ? nlohmann::json::object() : nlohmann::json::parse(body); }
        catch (...) { return nlohmann::json::object(); }
    };

    if (path == "/api/status" && method == "GET") return apiStatus();   // свой try_lock
    if (path == "/api/audit"  && method == "GET") return apiAudit();    // замок внутри Guardian
    if (method == "POST" && path.rfind("/api/lib/", 0) == 0) return apiLib(path.substr(9), parseBody());

    if (method == "POST" && path.rfind("/api/", 0) == 0) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (path == "/api/upload")  return apiUpload(parseBody());
        if (path == "/api/draft")   return apiDraft(parseBody());
        if (path == "/api/approve") return apiApprove();
        if (path == "/api/edit")    return apiEdit(parseBody());
        if (path == "/api/export")  return apiExport(parseBody());
    }
    return httpResponse(404, "text/plain", "Not Found");
}

// =========================================================
// API handlers
// =========================================================
std::string HttpServer::apiLib(const std::string& op, const nlohmann::json& b) {
    auto wrap = [&](const nlohmann::json& r) {
        if (r.is_object() && r.contains("error") && r["error"].is_string())
            return jsonError(r["error"].get<std::string>());
        return jsonOk(r);
    };
    auto id = [&]() -> long long {
        return (b.is_object() && b.contains("id") && b["id"].is_number_integer()) ? b["id"].get<long long>() : 0;
    };
    
    if (op == "scan") {
        bool started = indexer_.start();
        bool already_running = !started && indexer_.isRunning();
        return jsonOk({
            {"started", started},
            {"already_running", already_running}
        });
    }
    if (op == "scan_status")       return jsonOk(indexer_.status());
    if (op == "list")              return wrap(library_.list(b));
    if (op == "doc")               return wrap(library_.doc(id()));
    if (op == "update")            return wrap(library_.updateDoc(b));
    if (op == "collections")       return wrap(library_.collections());
    if (op == "collection_save")   return wrap(library_.saveCollection(b));
    if (op == "collection_delete") return wrap(library_.deleteCollection(b));
    if (op == "resort")            return wrap(library_.resort());
    if (op == "companies")         return wrap(library_.companies());
    if (op == "company_rename")    return wrap(library_.renameCompany(b));
    if (op == "profile_get")       return wrap(library_.profileGet());
    if (op == "profile_set")       return wrap(library_.profileSet(b));
    if (op == "cleanup")           return wrap(library_.cleanup());
    if (op == "ask")               return wrap(library_.ask(getStr(b, "question")));
    return httpResponse(404, "text/plain", "Not Found");
}

// Не блокируемся на mutex_: пока идёт долгая генерация, отвечаем "generating".
std::string HttpServer::apiStatus() {
    std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
    if (!lk.owns_lock()) return jsonOk({{"step","generating"}, {"model_loaded", model_.isLoaded()},
                                        {"pii_warning", false}, {"pending_approval", false}, {"error", ""}});

    const auto& s = task_.state();
    static const char* step_names[] = {"idle","extracted","draft_ready","approved","error"};
    nlohmann::json j;
    j["step"]             = step_names[static_cast<int>(s.step)];
    j["model_loaded"]     = model_.isLoaded();
    j["pii_warning"]      = s.pii_warning;
    j["pending_approval"] = guardian_.isPending();
    j["error"]            = s.error;
    return jsonOk(j);
}

std::string HttpServer::apiUpload(const nlohmann::json& body) {
    std::vector<std::string> paths;
    if (body.contains("paths") && body["paths"].is_array())
        for (const auto& p : body["paths"])
            if (p.is_string())   // только имя файла, всегда из inbox
                paths.push_back(workspace_ + "/inbox/" + std::filesystem::path(p.get<std::string>()).filename().string());
    if (paths.empty()) return jsonError("paths is empty");

    if (!task_.extract(paths)) return jsonError(task_.state().error);

    return jsonOk({
        {"full_text", task_.state().extracted},
        {"length", task_.state().extracted.size()}
    });
}

std::string HttpServer::apiDraft(const nlohmann::json& body) {
    std::string instruction = getStr(body, "instruction");
    if (!task_.draft(instruction)) return jsonError(task_.state().error);
    return jsonOk({
        {"draft",       task_.state().draft},
        {"pii_warning", task_.state().pii_warning},
        {"truncated",   task_.state().truncated}
    });
}

std::string HttpServer::apiApprove() {
    task_.approve();
    return jsonOk({{"step", "approved"}});
}

std::string HttpServer::apiEdit(const nlohmann::json& body) {
    std::string draft = getStr(body, "draft");
    if (draft.empty()) return jsonError("draft is empty");
    task_.editAndApprove(draft);
    return jsonOk({{"step", "approved"}, {"pii_warning", task_.state().pii_warning}});
}

std::string HttpServer::apiExport(const nlohmann::json& body) {
    std::string name = getStr(body, "name", "draft.md");
    name = std::filesystem::path(name).filename().string();
    for (char& c : name) if (!std::isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') c = '_';
    if (name.empty() || name[0] == '.') name = "draft.md";
    std::string output_path = workspace_ + "/output/" + name;
    if (!task_.exportTo(output_path)) return jsonError("Not approved, blocked by Guardian, or write error.");
    return jsonOk({{"saved_to", output_path}});
}

std::string HttpServer::apiAudit() {
    return jsonOk({{"logs", guardian_.logJson()}});
}

// =========================================================
// Helpers
// =========================================================

std::string HttpServer::loadHtml() const {
    std::ifstream f2(workspace_ + "/web/index.html");
    if (f2.is_open()) {
        std::stringstream ss;
        ss << f2.rdbuf();
        return ss.str();
    }
    return R"(<!DOCTYPE html><html><head><meta charset="utf-8">
            <title>Sift</title></head>
            <body style="font-family: system-ui, sans-serif; padding: 40px; color: #333; background: #f5f5f5;">
            <h2>index.html not found</h2>
            <p>Expected at: <code>)" + workspace_ + R"(/web/index.html</code></p>
            <p>Run <code>make install-web</code>.</p>
            </body></html>)";
}

std::string HttpServer::jsonOk(const nlohmann::json& data) {
    nlohmann::json j = {{"ok", true}};
    if (data.is_object())
        for (auto& [k, v] : data.items()) j[k] = v;
    return httpResponse(200, "application/json", j.dump());
}

std::string HttpServer::jsonError(const std::string& msg) {
    return httpResponse(200, "application/json",
                        nlohmann::json{{"ok", false}, {"error", msg}}.dump());
}

std::string HttpServer::httpResponse(int code,
                                     const std::string& content_type,
                                     const std::string& body) {
    static const std::unordered_map<int,std::string> phrases = {
        {200,"OK"},{400,"Bad Request"},{403,"Forbidden"},{404,"Not Found"},{500,"Internal Server Error"}
    };
    auto it = phrases.find(code);
    std::string phrase = it != phrases.end() ? it->second : "Unknown";

    std::ostringstream ss;
    ss << "HTTP/1.1 " << code << " " << phrase << "\r\n"
       << "Content-Type: " << content_type << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "X-Content-Type-Options: nosniff\r\n"
       << "Content-Security-Policy: default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; connect-src 'self'\r\n"
       << "Cache-Control: no-store\r\n"
       << "Connection: close\r\n\r\n"
       << body;
    return ss.str();
}