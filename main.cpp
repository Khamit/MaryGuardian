#include "core/Guardian.hpp"
#include "core/ModelRuntime.hpp"
#include "core/Verifier.hpp"
#include "task/BookkeeperTask.hpp"
#include "server/HttpServer.hpp"

#include "core/Db.hpp"
#include "core/Indexer.hpp"
#include "core/Library.hpp"

#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <unistd.h>

std::atomic<bool> g_running(true);

static void signalHandler(int) {
    static const char msg[] = "\n[Main] Stopping...\n";
    (void)!write(STDOUT_FILENO, msg, sizeof msg - 1);   // cout в обработчике сигнала небезопасен
    g_running = false;
}

int main(int argc, char* argv[]) {
    // sigaction без SA_RESTART: Ctrl+C прерывает getline.
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);   // клиент закрыл сокет — не падаем

    std::string workspace  = "mg_workspace";
    int         port       = 8080;
    std::string model_path;
    std::string ocr_model;  
    std::string ocr_mmproj; 
    
    // цикл разбора аргументов
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        try {
            if (a == "--workspace" && i + 1 < argc)  workspace  = argv[++i];
            else if (a == "--port" && i + 1 < argc)  port       = std::stoi(argv[++i]);
            else if (a == "--model" && i + 1 < argc) model_path = argv[++i];
            else if (a == "--ocr-model" && i + 1 < argc) ocr_model = argv[++i];       // <-- ДОБАВИТЬ
            else if (a == "--ocr-mmproj" && i + 1 < argc) ocr_mmproj = argv[++i];     // <-- ДОБАВИТЬ
        } catch (const std::exception&) {
            std::cerr << "[Main] Invalid value for " << a << "\n";
            return 2;
        }
    }

    std::filesystem::create_directories(workspace + "/logs");
    std::filesystem::create_directories(workspace + "/output");
    std::filesystem::create_directories(workspace + "/web");

    Guardian guardian(workspace + "/logs/audit.jsonl", workspace + "/inbox", workspace + "/output");
    mary::ModelRuntime model;
    mary::Verifier    verifier;
    mary::BookkeeperTask task(model, guardian, verifier);

    mary::Db db(workspace + "/library.db");
    if (!db.ok()) { std::cerr << "[Main] Cannot open library.db\n"; return 1; }
    // Передаем пути к OCR в Indexer
    mary::Indexer indexer(db, guardian, model, verifier, workspace + "/inbox", ocr_model, ocr_mmproj);
    mary::Library library(db, guardian, model);

    if (!model_path.empty()) {
        if (!std::filesystem::exists(model_path)) {
            std::cerr << "[Main] WARNING: model file not found: " << model_path << "\n";
        } else if (model.loadModel(model_path, 8192, 99)) {
            std::cout << "[Main] Model loaded: " << model_path << "\n";
        } else {
            std::cerr << "[Main] WARNING: model not loaded — draft will fail\n";
        }
    } else {
        std::cout << "[Main] No --model specified. Draft unavailable.\n";
    }

        HttpServer server(port, task, guardian, model, indexer, library, workspace);
    server.setRunningFlag(&g_running);

    if (!server.start()) {
        std::cerr << "[Main] Failed to start HTTP server\n";
        return 1;
    }
    
    indexer.start();   // известные файлы пропускаются, новые разбираются в фоне

    std::cout << "\n=========================================\n"
              << "  MaryGuardian\n"
              << "  UI:  http://localhost:" << port << "\n"
              << "=========================================\n";

    if (isatty(STDIN_FILENO)) {
        std::cout << "Type 'quit' to exit\n\n";
        std::string cmd;
        while (g_running) {
            std::cout << "> ";
            std::cout.flush();
            if (!std::getline(std::cin, cmd) || cmd == "quit" || cmd == "exit") break;
        }
    } else {
        // Запуск без терминала (launchd, nohup): EOF на stdin не должен останавливать сервер.
        while (g_running) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    g_running = false;

    server.stop();
    indexer.stop();
    model.unload();
    std::cout << "[Main] Done.\n";
    return 0;
}