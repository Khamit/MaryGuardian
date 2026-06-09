// server/ApiHandlers.cpp
#include "ApiHandlers.hpp"
#include "core/NeuralFieldSystem.hpp"
#include "core/AgentAuditBridge.hpp"
#include "server/AgentRegistry.hpp"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iostream>
#include <unistd.h>

ApiHandlers::ApiHandlers(NeuralFieldSystem* nfs, AgentAuditBridge* auditor, const std::string& workspace)
    : nfs_(nfs), auditor_(auditor), workspace_(workspace) {}

std::string ApiHandlers::handleStatus() {
    std::string status_file = workspace_ + "/agent_status.json";
    std::ifstream file(status_file);
    if (!nfs_) return R"({"status":"error"})";
    if (file.is_open()) {
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
    
    if (!auditor_) {
        return R"({"status":"waiting","risk":0,"entropy":0.5,"quality":0.5,"surprise":0})";
    }
    auto snap = nfs_->getSystemSnapshot();
    auto& session = auditor_->getCurrentSession();

        nlohmann::json j;
    j["status"] = "active";
    j["session_id"] = session.session_id;
    j["total_steps"] = session.total_steps;
    j["risk"] = auditor_->getCurrentHallucinationRisk();
    j["entropy"] = snap.entropy;                    // из модели
    j["quality"] = snap.quality;                    // из модели (lastSignal_.quality)
    j["surprise"] = snap.surprise;                  // из модели
    j["energy"] = snap.energy;                      // из LagrangianAuditor
    j["energy_error"] = snap.energy_error;          // реальная ошибка
    j["temperature"] = snap.temperature;            // реальная температура
    j["stm_size"] = snap.stm_size;                  // реальный размер STM
    j["ltm_size"] = snap.ltm_size;                  // реальный размер LTM
    j["violations"] = snap.violations;              // реальные нарушения
    j["is_dangerous_mode"] = (snap.entropy > 0.7);
    
    return j.dump();
}

std::string ApiHandlers::handleConstraints() {
    if (!auditor_) {
        return "{}";
    }
    auto constraints = auditor_->getConstraintsStatus();
    nlohmann::json j = constraints;
    return j.dump();
}

std::string ApiHandlers::handleAuditorStatus() {
    if (!nfs_) {
        return "{}";
    }
    nlohmann::json j;
    auto& auditor = nfs_->getLagrangianAuditor();
    auto& config = auditor.getConfig();
    j["enabled"] = nfs_->isEnergyAuditEnabled();
    j["energy"] = auditor.getReferenceEnergy();
    j["energy_error"] = auditor.getEnergyError();
    j["energy_error_ema"] = auditor.getEnergyError();
    j["violations"] = auditor.getConservationViolations();
    j["reference_energy"] = auditor.getReferenceEnergy();
    j["threshold_energy"] = config.energy_conservation_threshold;
    j["threshold_momentum"] = config.momentum_conservation_threshold;
    j["threshold_action"] = config.action_threshold;
    j["auto_correct"] = config.auto_correct;
    j["correction_strength"] = config.correction_strength;
    return j.dump();
}

std::string ApiHandlers::handleAnalytics() {
    nlohmann::json j;
    j["active_orchestrations"] = 1;
    j["total_agents"] = 1;
    j["active_agents"] = auditor_ ? 1 : 0;
    j["total_actions"] = auditor_ ? auditor_->getCurrentSession().total_steps : 0;
    j["success_rate"] = 0.8;
    j["low_risk_count"] = 5;
    j["medium_risk_count"] = 3;
    j["high_risk_count"] = 1;
    j["timeline"] = {{"labels", {"00", "01", "02", "03", "04", "05", "06", "07", "08", "09", "10", "11", "12", "13", "14", "15", "16", "17", "18", "19", "20", "21", "22", "23"}}, 
                      {"values", {2,1,0,0,0,0,1,3,5,8,12,15,18,22,25,28,30,35,38,42,45,48,50,45}}};
    j["tools_usage"] = nlohmann::json::array();
    j["tools_usage"].push_back({{"name", "run_sql"}, {"count", 45}});
    j["tools_usage"].push_back({{"name", "calculate"}, {"count", 23}});
    j["tools_usage"].push_back({{"name", "respond"}, {"count", 67}});
    j["orchestrations"] = nlohmann::json::array();
    j["agents"] = nlohmann::json::array();
    
    if (auditor_) {
        nlohmann::json orch;
        orch["id"] = "main_orch";
        orch["type"] = "demo";
        orch["agents"] = 1;
        orch["status"] = "active";
        orch["avg_risk"] = auditor_->getCurrentHallucinationRisk();
        orch["actions"] = auditor_->getCurrentSession().total_steps;
        j["orchestrations"].push_back(orch);
        
        nlohmann::json agent;
        agent["id"] = auditor_->getCurrentSession().session_id;
        agent["type"] = "llm";
        agent["orchestration"] = "main_orch";
        agent["status"] = "active";
        agent["risk"] = auditor_->getCurrentHallucinationRisk();
        agent["entropy"] = auditor_->getCurrentEntropy();
        agent["last_active"] = std::to_string(time(nullptr));
        j["agents"].push_back(agent);
    }
    return j.dump();
}

std::string ApiHandlers::handleAgentsList() {
    return AgentRegistry::getInstance().getAllAgents().dump();
}

std::string ApiHandlers::handleDemoStatus() {
    std::string status_file = workspace_ + "/agent_status.json";
    std::ifstream file(status_file);
    nlohmann::json j;
    if (file.is_open()) {
        file >> j;
    } else {
        j["status"] = "stopped";
        j["risk"] = 0;
        j["steps"] = 0;
    }
    return j.dump();
}

std::string ApiHandlers::handleLastAction() {
    if (!auditor_) {
        return "{}";
    }
    auto info = auditor_->getLastActionInfo();
    return info.toJson().dump();
}

std::string ApiHandlers::handleRegisterAgent(const nlohmann::json& body) {
    std::string agent_id = body.value("agent_id", "");
    std::string type = body.value("type", "generic");
    std::string endpoint = body.value("endpoint", "");
    
    if (agent_id.empty()) {
        return R"({"error":"agent_id required"})";
    }
    
    AgentRegistry::getInstance().registerAgent(agent_id, type, endpoint);
    nlohmann::json result = {{"status", "ok"}, {"agent_id", agent_id}};
    return result.dump();
}

std::string ApiHandlers::handleHeartbeat(const nlohmann::json& body) {
    std::string agent_id = body.value("agent_id", "");
    if (agent_id.empty()) {
        return R"({"error":"agent_id required"})";
    }
    
    AgentRegistry::getInstance().heartbeat(agent_id);
    return R"({"status":"ok"})";
}

std::string ApiHandlers::handleAuditAction(const nlohmann::json& body) {
    if (!auditor_ || !nfs_) {
        return R"({"allowed":true,"risk":0,"reason":"Auditor not available"})";
    }
    
    std::string agent_id = body.value("agent_id", "");
    std::string action = body.value("action", "");
    std::string tool_name = body.value("tool_name", "");
    std::string thought = body.value("thought", "");
    float confidence = body.value("confidence", 0.5f);
    int step = body.value("step", 0);
    
    AgentAction audit_action;
    audit_action.action = action;
    audit_action.tool_name = tool_name;
    audit_action.thought = thought;
    audit_action.step_number = step;
    audit_action.action_id = "api_" + std::to_string(time(nullptr));
    
    AuditVerdict verdict = auditor_->auditAction(audit_action);
    
    AgentRegistry::getInstance().recordAction(agent_id, verdict.hallucination_risk, verdict.allowed);
    
    nlohmann::json result;
    result["allowed"] = verdict.allowed;
    result["risk"] = verdict.hallucination_risk;
    result["reason"] = verdict.reason;
    result["suggested_action"] = verdict.suggested_action;
    
    return result.dump();
}

std::string ApiHandlers::handleConstraintToggle(const nlohmann::json& body) {
    if (!auditor_) {
        return R"({"status":"error","reason":"Auditor not available"})";
    }
    
    std::string name = body.value("name", "");
    bool enabled = body.value("enabled", false);
    
    if (name.empty()) {
        return R"({"error":"name required"})";
    }
    
    auditor_->enableConstraint(name, enabled);
    std::cout << "[API] Constraint " << name << " " << (enabled ? "enabled" : "disabled") << std::endl;
    
    return R"({"status":"ok"})";
}

std::string ApiHandlers::handleSessionStart(const nlohmann::json& body) {
    if (!auditor_) {
        return R"({"status":"error","reason":"Auditor not available"})";
    }
    
    std::string session_id = body.value("session_id", "");
    std::string agent_name = body.value("agent_name", "");
    
    if (session_id.empty()) {
        session_id = "session_" + std::to_string(time(nullptr));
    }
    if (agent_name.empty()) {
        agent_name = "unknown";
    }
    
    auditor_->startSession(session_id, agent_name);
    std::cout << "[API] Session started: " << session_id << std::endl;
    
    return R"({"status":"ok"})";
}

std::string ApiHandlers::handleSessionEnd() {
    if (!auditor_) {
        return R"({"status":"error","reason":"Auditor not available"})";
    }
    
    auditor_->endSession();
    std::cout << "[API] Session ended" << std::endl;
    
    return R"({"status":"ok"})";
}

std::string ApiHandlers::handleSaveState() {
    if (!auditor_) {
        return R"({"status":"error","reason":"Auditor not available"})";
    }
    
    auditor_->saveMemoryState();
    std::cout << "[API] State saved" << std::endl;
    
    return R"({"status":"ok"})";
}

std::string ApiHandlers::handleResetRisk() {
    if (!auditor_) {
        return R"({"status":"error","reason":"Auditor not available"})";
    }
    
    auditor_->resetRiskAccumulator();
    std::cout << "[API] Risk reset" << std::endl;
    
    return R"({"status":"ok"})";
}

std::string ApiHandlers::handleAuditorToggle(const nlohmann::json& body) {
    if (!nfs_) {
        return R"({"status":"error","reason":"NFS not available"})";
    }
    
    bool enabled = body.value("enabled", true);
    nfs_->setEnergyAuditEnabled(enabled);
    std::cout << "[API] Energy audit " << (enabled ? "enabled" : "disabled") << std::endl;
    
    return R"({"status":"ok"})";
}

std::string ApiHandlers::handleAuditorReset() {
    if (!nfs_) {
        return R"({"status":"error","reason":"NFS not available"})";
    }
    
    nfs_->getLagrangianAuditorNonConst().reset();
    std::cout << "[API] Auditor reset" << std::endl;
    
    return R"({"status":"ok"})";
}

std::string ApiHandlers::handleAuditorConfigSave(const nlohmann::json& body) {
    if (!nfs_) {
        return R"({"status":"error","reason":"NFS not available"})";
    }
    
    auto& config = nfs_->getLagrangianAuditorNonConst().getConfigNonConst();
    config.energy_conservation_threshold = body.value("energy_threshold", 0.05);
    config.momentum_conservation_threshold = body.value("momentum_threshold", 0.10);
    config.action_threshold = body.value("action_threshold", 0.15);
    config.auto_correct = body.value("auto_correct", true);
    config.correction_strength = body.value("correction_strength", 0.1);
    
    std::cout << "[API] Auditor config saved" << std::endl;
    
    return R"({"status":"ok"})";
}

std::string ApiHandlers::handleDemoStart() {
    // Проверяем, не запущен ли уже демо-агент
    std::string pid_file = workspace_ + "/demo.pid";
    std::ifstream pid_check(pid_file);
    if (pid_check.is_open()) {
        int pid;
        pid_check >> pid;
        pid_check.close();
        
        // Проверяем, жив ли процесс
        std::string check_cmd = "kill -0 " + std::to_string(pid) + " 2>/dev/null";
        if (system(check_cmd.c_str()) == 0) {
            std::cout << "[API] Demo agent already running (PID: " << pid << ")" << std::endl;
            return R"({"status":"already_running"})";
        }
    }
    
    std::string cmd = "python3 demo_runner.py start --workspace " + workspace_ + " > demo.log 2>&1 &";
    if (access("demo_runner.py", F_OK) != 0) {
        cmd = "cd " + workspace_ + " && python3 ../demo_runner.py start --workspace " + workspace_ + " > demo.log 2>&1 &";
    }
    int result = system(cmd.c_str());
    std::cout << "[API] Demo start result: " << result << std::endl;
    return R"({"status":"ok"})";
}

std::string ApiHandlers::handleDemoStop() {
    std::string cmd = "pkill -f 'demo_runner.py start'";
    system(cmd.c_str());
    return R"({"status":"ok"})";
}

std::string ApiHandlers::handleDemoScenario() {
    std::string cmd = "python3 demo_runner.py scenario --workspace " + workspace_;
    if (access("demo_runner.py", F_OK) != 0) {
        cmd = "cd " + workspace_ + " && python3 ../demo_runner.py scenario --workspace " + workspace_;
    }
    system(cmd.c_str());
    nlohmann::json result = {{"status", "ok"}, {"actions", 5}};
    return result.dump();
}

std::string ApiHandlers::handleStats(const std::string& period) {
    if (!auditor_) {
        return "{}";
    }
    auto stats = auditor_->getLogger().getStatsForPeriod(period);
    nlohmann::json j = stats.toJson();
    return j.dump();
}

std::string ApiHandlers::handleMetrics() {
    if (!nfs_) {
        return "{}";
    }
    auto snap = nfs_->getSystemSnapshot();
    auto j = snap.toJson();
    if (auditor_) {
        j["constraints"] = auditor_->getConstraintsStatus();
        auto last = auditor_->getLastActionInfo();
        j["last_action"] = {
            {"action", last.action},
            {"risk", last.risk},
            {"allowed", last.allowed}
        };
    }
    return j.dump();
}

nlohmann::json ApiHandlers::getEventData() {
    nlohmann::json event;
    
    if (nfs_) {
        auto snap = nfs_->getSystemSnapshot();  // ИСТОЧНИК ПРАВДЫ
        
        event["energy_error"] = snap.energy_error;
        event["violations"] = snap.violations;
        event["entropy"] = snap.entropy;
        event["quality"] = snap.quality;
        event["temperature"] = snap.temperature;
        event["stm_size"] = snap.stm_size;
        event["ltm_size"] = snap.ltm_size;
        event["surprise"] = snap.surprise;
        event["system_energy"] = snap.energy;
        
        // Данные от SelfSignalSampler (32 сигнала)
        auto& sampler = nfs_->getSelfSignalSampler();  // Нужно добавить геттер
        auto& lastSnap = sampler.last();
        event["self_sensory_rate"] = lastSnap.sensory_avg_rate;
        event["self_motor_rate"] = lastSnap.motor_avg_rate;
        event["self_entropy"] = lastSnap.system_entropy;
        event["self_apoptosis"] = lastSnap.apoptosis_rate;
        event["self_neurogenesis"] = lastSnap.neurogenesis_rate;
    }
    
    if (auditor_) {
        event["risk"] = auditor_->getCurrentHallucinationRisk();
        event["session_id"] = auditor_->getCurrentSession().session_id;
    }
    
    return event;
}