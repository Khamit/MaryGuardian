#pragma once

#include "NeuralGroup.hpp"
#include "EmergentCore.hpp"
#include <vector>
#include <random>
#include <string>
#include <deque>
#include <algorithm>
#include <cmath>
#include "INeuralGroupAccess.hpp"
#include <mutex>
#include <memory>
#include "LagrangianAuditor.hpp"
#include <map>
#include <nlohmann/json.hpp>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "SelfSignalSampler.hpp"

// ──────────────────────────────────────────────────────────────────────────────
// AttentionMechanism — упрощённая версия (только softmax)
// ──────────────────────────────────────────────────────────────────────────────
struct AttentionMechanism {
    std::vector<double> attention_weights;
    std::vector<float>  context_vector;
    float  temperature = 1.0f;
    double entropy     = 0.0;

    void computeAttention(const std::vector<double>& groupActivities) {
        const size_t n = groupActivities.size();
        attention_weights.resize(n);
        if (n == 0) return;

        float maxVal = static_cast<float>(
            *std::max_element(groupActivities.begin(), groupActivities.end()));
        const float temp = std::max(temperature, 1e-6f);

        float sum = 0.f;
        for (size_t i = 0; i < n; ++i) {
            float v = std::exp((static_cast<float>(groupActivities[i]) - maxVal) / temp);
            attention_weights[i] = v;
            sum += v;
        }
        if (sum < 1e-12f) {
            float u = 1.f / n;
            for (auto& w : attention_weights) w = u;
            entropy = std::log(static_cast<double>(n));
            return;
        }
        float inv = 1.f / sum;
        entropy = 0.0;
        for (auto& w : attention_weights) {
            w *= inv;
            if (w > 1e-12) entropy -= w * std::log(static_cast<double>(w));
        }
    }
};

// НУЖНО ДОБАВИТЬ в NeuralFieldSystem.hpp:

struct AgentMetrics {
    // Trajectory Metrics
    double action_validity_rate = 0.0;      // AVR
    double tool_success_rate = 0.0;          // TSR
    double tool_result_grounding = 0.0;      // TRG
    double plan_execute_divergence = 0.0;    // PED
    double recovery_rate = 0.0;              // RR
    
    // Grounding Metrics
    double context_utilization_rate = 0.0;   // CUR
    double retrieval_precision = 0.0;
    double retrieval_recall = 0.0;
    double attribution_score = 0.0;          // AS
    double unsupported_claim_rate = 0.0;     // UCR
    
    // Calibration Metrics
    double expected_calibration_error = 0.0; // ECE
    double selective_risk = 0.0;
    double uncertainty_gap = 0.0;
    
    // Multi-Agent Metrics
    double inter_agent_consistency = 0.0;    // IAC
    double critic_correction_rate = 0.0;     // CCR
    double debate_convergence_rate = 0.0;
    double policy_compliance_rate = 0.0;     // PCR
    
    // Stability Metrics
    double trajectory_entropy = 0.0;         // TE
    double state_drift = 0.0;                // SD
    double looping_rate = 0.0;
    double dead_end_probability = 0.0;
    
    // Production Metrics
    double cost_per_successful_task = 0.0;   // CST
    double latency_to_resolution = 0.0;      // LTR
    double tool_call_efficiency = 0.0;       // TCE
    
    void toJson(nlohmann::json& j) const {
        j["AVR"] = action_validity_rate;
        j["TSR"] = tool_success_rate;
        j["TRG"] = tool_result_grounding;
        j["PED"] = plan_execute_divergence;
        j["RR"] = recovery_rate;
        j["CUR"] = context_utilization_rate;
        j["AS"] = attribution_score;
        j["UCR"] = unsupported_claim_rate;
        j["ECE"] = expected_calibration_error;
        j["IAC"] = inter_agent_consistency;
        j["PCR"] = policy_compliance_rate;
        j["TE"] = trajectory_entropy;
        j["SD"] = state_drift;
        j["LTR"] = latency_to_resolution;
        j["TCE"] = tool_call_efficiency;
    }
};

// ──────────────────────────────────────────────────────────────────────────────
// NeuralFieldSystem — новая версия без орбит
// ──────────────────────────────────────────────────────────────────────────────
class NeuralFieldSystem : public INeuralGroupAccess {
public:
    static constexpr int NUM_GROUPS   = 32;
    static constexpr int GROUP_SIZE   = 32;
    static constexpr int TOTAL_NEURONS = NUM_GROUPS * GROUP_SIZE;

    // Роли групп (фиксированные)
    static constexpr int INPUT_GROUP       = 0;
    static constexpr int SENSORY_START     = 1;
    static constexpr int SENSORY_END       = 3;
    static constexpr int MOTOR_START       = 4;
    static constexpr int MOTOR_END         = 7;
    static constexpr int ASSOCIATIVE_START = 8;
    static constexpr int ASSOCIATIVE_END   = 15;
    static constexpr int SEMANTIC_START    = 16;
    static constexpr int SEMANTIC_END      = 21;
    static constexpr int CONTEXT_START     = 22;
    static constexpr int CONTEXT_END       = 27;
    static constexpr int SELF_MODEL_START  = 28;
    static constexpr int SELF_MODEL_END    = 31;

    NeuralFieldSystem(double dt);
    ~NeuralFieldSystem() = default;

    void initialize(std::mt19937& rng);
    
    NeuralFieldSystem(const NeuralFieldSystem&) = delete;
    NeuralFieldSystem& operator=(const NeuralFieldSystem&) = delete;
    
    // ==== Аудит агентов ==== //
    // Новый метод для логирования шага агента
    void ingestAgentStep(const std::string& thought, 
                        const std::string& action, 
                        const std::string& observation,
                        const std::string& toolName,
                        bool stepSuccessful);

    void step(float external_reward, int stepNumber);
    int getCurrentStep() const { return stepCounter; }

    // Состояние системы
    const std::vector<double>& getPhi() const { rebuildFlatVectors(); return flatPhi; }
    const std::vector<double>& getPi()  const { rebuildFlatVectors(); return flatPi; }
    std::vector<double> getGroupAverages() const;
    const std::vector<std::vector<double>>& getInterWeights() const { return interWeights; }
    
    // Энтропия и энергия (упрощённые)
    double computeSystemEntropy() const;
    double getUnifiedEntropy() const;
    double getTargetUnifiedEntropy() const;

    // Emergent компоненты
    const EmergentSignal& lastSignal() const { return lastSignal_; }
    const EmergentController& emergent() const { return emergent_; }
    EmergentController& emergentMutable() { return emergent_; }

    // Межгрупповые связи
    void strengthenInterConnection(int from, int to, double delta);
    void weakenInterConnection(int from, int to, double delta);

    // Доступ к группам
    std::vector<NeuralGroup>& getGroupsNonConst() { return groups; }
    const std::vector<NeuralGroup>& getGroups()   const { return groups; }
    
    std::vector<float> getFeatures() const;

    // Режимы работы
    void setOperatingMode(OperatingMode::Type mode);
    bool isTrainingMode() const { return training_mode_; }
    void setTrainingMode(bool e) { training_mode_ = e; }

    // Внимание
    float  getAttentionTemperature() const { return attention.temperature; }
    void   setAttentionTemperature(float t) { attention.temperature = t; }
    double getAttentionEntropy() const { return attention.entropy; }
    const std::vector<double>& getAttentionWeights() const { return attention.attention_weights; }

    // Управление памятью
    void setMemoryManager(EmergentMemory* mm) { memory_manager = mm; }
    
    // Внешние воздействия
    void applyResourcePenalty(float penalty);
    void enterLowPowerMode();
    void applyTargetPattern(const std::vector<float>& target);
    void applyTargetedMutation(double strength, int targetType);
    void setInputText(const std::vector<float>& sig);
    void addExternalInput(const std::vector<float>& in) { external_inputs_ = in; }
    void clearExternalInputs() { external_inputs_.clear(); }
    
    // Диагностика
    void diagnoseCriticality();
    void logSystemHealth();
    double computeOptimalStructure();

    // Рефлексия (заглушки для совместимости)
    struct ReflectionState {
        double confidence, curiosity, satisfaction, confusion;
        std::vector<double> attention_map;
    };
    ReflectionState getReflectionState() const;
    void reflect();
    void setGoal(const std::string& goal);
    bool evaluateProgress();
    void learnFromReflection(float outcome);

    // INeuralGroupAccess
    std::vector<NeuralGroup*>& getHubGroups() override {
        static std::vector<NeuralGroup*> ptrs;
        ptrs.clear();
        for (int idx : hubIndices)
            if (idx >= 0 && idx < (int)groups.size()) ptrs.push_back(&groups[idx]);
        return ptrs;
    }
    const std::vector<NeuralGroup*>& getHubGroups() const override {
        static std::vector<NeuralGroup*> ptrs;
        ptrs.clear();
        for (int idx : hubIndices)
            if (idx >= 0 && idx < (int)groups.size())
                ptrs.push_back(const_cast<NeuralGroup*>(&groups[idx]));
        return ptrs;
    }
    void registerHub(int g) override {
        if (g >= 0 && g < (int)groups.size()) hubIndices.push_back(g);
    }

    // НОВЫЕ МЕТОДЫ:
    void setEnergyAuditEnabled(bool enabled) { energy_audit_enabled_ = enabled; }
    bool isEnergyAuditEnabled() const { return energy_audit_enabled_; }
    const LagrangianAuditor& getLagrangianAuditor() const { return lagrangian_auditor_; }
    LagrangianAuditor& getLagrangianAuditorNonConst() { return lagrangian_auditor_; }
    const CanonicalState& getCanonicalState() const { return canonical_state_; }
    const SelfSignalSampler& getSelfSignalSampler() const { return self_sampler_; }
    SelfSignalSampler& getSelfSignalSamplerNonConst() { return self_sampler_; }

    // Потокобезопасность
    void lock()   { system_mutex_.lock(); }
    void unlock() { system_mutex_.unlock(); }
    std::mutex& getMutex() { return system_mutex_; }

    float getEnergyBasedHallucinationRisk() const {
        return static_cast<float>(lagrangian_auditor_.getEnergyError());
    }

    class ScopedLock {
        NeuralFieldSystem& s_;
    public:
        ScopedLock(NeuralFieldSystem& s) : s_(s) { s_.lock(); }
        ~ScopedLock() { s_.unlock(); }
    };

    // ==== API ДЛЯ HTTP СЕРВЕРА ====
    struct SystemSnapshot {
        double energy = 0.0;
        double kinetic = 0.0;
        double potential = 0.0;
        double energy_error = 0.0;
        double entropy = 0.0;
        double target_entropy = 0.5;
        double surprise = 0.0;
        double quality = 0.0;
        double temperature = 1.0;
        size_t stm_size = 0;
        size_t ltm_size = 0;
        int violations = 0;
        std::map<std::string, bool> constraints;
        
        nlohmann::json toJson() const {
            nlohmann::json j;
            j["energy"] = energy;
            j["kinetic"] = kinetic;
            j["potential"] = potential;
            j["energy_error"] = energy_error;
            j["entropy"] = entropy;
            j["target_entropy"] = target_entropy;
            j["surprise"] = surprise;
            j["quality"] = quality;
            j["temperature"] = temperature;
            j["stm_size"] = stm_size;
            j["ltm_size"] = ltm_size;
            j["violations"] = violations;
            j["constraints"] = constraints;
            return j;
        }
    };
    
    SystemSnapshot getSystemSnapshot() const {
        SystemSnapshot snap;
        snap.energy = lagrangian_auditor_.getReferenceEnergy();
        snap.kinetic = lagrangian_auditor_.getLastKinetic();    
        snap.potential = lagrangian_auditor_.getLastPotential();
        snap.energy_error = lagrangian_auditor_.getEnergyError();
        snap.violations = static_cast<int>(lagrangian_auditor_.getConservationViolations());
        snap.entropy = getUnifiedEntropy();
        snap.target_entropy = getTargetUnifiedEntropy();
        snap.surprise = lastSignal_.surprise;
        snap.quality = lastSignal_.quality;
        snap.temperature = attention.temperature;
        snap.stm_size = emergent_.memory.stmSize();
        snap.ltm_size = emergent_.memory.ltmSize();
        // constraints нужно заполнить извне или добавить поле в EmergentSignal
        return snap;
    }

private:
    // Основные компоненты
    EmergentController emergent_;
    EmergentSignal     lastSignal_;
    AttentionMechanism attention;
    OperatingMode::Type current_mode_ = OperatingMode::NORMAL;
    EmergentMemory*     memory_manager = nullptr;
    
    // Self-model
    SelfSignalSampler self_sampler_;
    
    // Мьютекс
    mutable std::mutex system_mutex_;

    // Группы нейронов
    double dt_;
    std::vector<NeuralGroup> groups;
    std::vector<std::vector<double>> interWeights;

    // Кэши для внешнего доступа
    mutable std::vector<double> flatPhi, flatPi;
    mutable bool flatDirty = true;
    void rebuildFlatVectors() const;

    // Счётчики и состояние
    int stepCounter = 0;
    bool training_mode_ = false;
    bool pendingEvolution_ = false;
    std::vector<int> hubIndices;
    std::vector<float> external_inputs_;
    std::string current_goal;
    
    // История энтропии
    std::deque<double> entropy_history;
    static constexpr int HISTORY_SIZE = 200;

    // Приватные методы
    void applyLateralInhibition();
    void applyPruningByElevation();
    void consolidateInterWeights(float pressure);
    void routeRewards(const EmergentSignal& sig, int step);
    
    // Вспомогательные
    void setupFixedInterConnections();
    void setupGroupSpecializations();
    
    // Устаревшие методы (заглушки для совместимости)
    void applyReentry(int) {}
    void applyRicciFlow() {}
    void maintainCriticality() {}
    void logOrbitalHealth() { logSystemHealth(); }
    bool checkForResourceExhaustion() { return false; }
    void generateGentleBurst() {}
    float computeGlobalImportance() { return 0.5f; }
    double computeTotalEnergy() const { return 0.0; }  // устарело

    // НОВОЕ: Lagrangian аудитор
    LagrangianAuditor lagrangian_auditor_;
    
    // НОВОЕ: каноническое состояние для отслеживания
    CanonicalState canonical_state_;
    CanonicalState previous_canonical_state_;
    
    // НОВОЕ: включение/выключение аудита
    bool energy_audit_enabled_ = true;
};