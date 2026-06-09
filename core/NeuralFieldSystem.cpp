#include "NeuralFieldSystem.hpp"
#include <cmath>
#include <iostream>
#include <numeric>
#include <algorithm>
#include <iomanip>
#include <random>
#include <map>
#include <string>
#include "LagrangianAuditor.hpp"

// Константы
static constexpr int MIN_CONSOLIDATION_INTERVAL = 20;
static constexpr int EVOLUTION_INTERVAL = 5000;

// ============================================================================
// КОНСТРУКТОР И ИНИЦИАЛИЗАЦИЯ
// ============================================================================

NeuralFieldSystem::NeuralFieldSystem(double dt)
    : dt_(dt),
      groups(),
      interWeights(NUM_GROUPS, std::vector<double>(NUM_GROUPS, 0.0)),
      flatPhi(TOTAL_NEURONS, 0.0),
      flatPi(TOTAL_NEURONS, 0.0),
      flatDirty(true)
{}

void NeuralFieldSystem::initialize(std::mt19937& rng) {
    // Создаём группы с новой сигнатурой (без MassLimits)
    auto shared_rng = std::make_shared<std::mt19937>(rng);
    groups.clear();
    groups.reserve(NUM_GROUPS);
    for (int g = 0; g < NUM_GROUPS; ++g) {
        groups.emplace_back(GROUP_SIZE, dt_, shared_rng);
    }
    
    // Настраиваем специализации групп
    setupGroupSpecializations();
    
    // Устанавливаем менеджер памяти
    for (auto& group : groups) {
        group.setMemoryManager(memory_manager);
    }
    
    // Настраиваем фиксированные межгрупповые связи
    setupFixedInterConnections();
    
    flatDirty = true;
    
    std::cout << "[NeuralFieldSystem] Initialized with " << NUM_GROUPS 
              << " groups of " << GROUP_SIZE << " neurons" << std::endl;
}

void NeuralFieldSystem::setupGroupSpecializations() {
    // Группа 0 — входная
    groups[INPUT_GROUP].setInputGroup(true);
    groups[INPUT_GROUP].setSpecialization("input");
    
    // Сенсорные группы (1-3)
    for (int g = SENSORY_START; g <= SENSORY_END; ++g) {
        groups[g].setSpecialization("sensory");
    }
    
    // Моторные группы (4-7) — действия
    for (int g = MOTOR_START; g <= MOTOR_END; ++g) {
        groups[g].setSpecialization("motor");
    }
    
    // Ассоциативные группы (8-15)
    for (int g = ASSOCIATIVE_START; g <= ASSOCIATIVE_END; ++g) {
        groups[g].setSpecialization("associative");
    }
    
    // Семантические группы (16-21) — смыслы
    for (int g = SEMANTIC_START; g <= SEMANTIC_END; ++g) {
        groups[g].setSpecialization("semantic");
    }
    
    // Контекстные группы (22-27)
    for (int g = CONTEXT_START; g <= CONTEXT_END; ++g) {
        groups[g].setSpecialization("context");
    }
    
    // Self-model группы (28-31)
    for (int g = SELF_MODEL_START; g <= SELF_MODEL_END; ++g) {
        groups[g].setSpecialization("self_model");
        groups[g].setSelfModelGroup(true);
    }
}

void NeuralFieldSystem::setupFixedInterConnections() {
    // Обнуляем все связи
    interWeights.assign(NUM_GROUPS, std::vector<double>(NUM_GROUPS, 0.0));
    
    // 1. Вход → сенсорика
    for (int s = SENSORY_START; s <= SENSORY_END; ++s) {
        interWeights[INPUT_GROUP][s] = 0.8;
        interWeights[s][INPUT_GROUP] = 0.2;
    }
    
    // 2. Сенсорика → ассоциативные
    for (int s = SENSORY_START; s <= SENSORY_END; ++s) {
        for (int a = ASSOCIATIVE_START; a <= ASSOCIATIVE_END; ++a) {
            interWeights[s][a] = 0.5;
        }
    }
    
    // 3. Ассоциативные → семантические
    for (int a = ASSOCIATIVE_START; a <= ASSOCIATIVE_END; ++a) {
        for (int sem = SEMANTIC_START; sem <= SEMANTIC_END; ++sem) {
            interWeights[a][sem] = 0.4;
        }
    }
    
    // 4. Семантические → моторные (действия)
    for (int sem = SEMANTIC_START; sem <= SEMANTIC_END; ++sem) {
        for (int m = MOTOR_START; m <= MOTOR_END; ++m) {
            interWeights[sem][m] = 0.3;
        }
    }
    
    // 5. Контекст → все (кроме себя)
    for (int ctx = CONTEXT_START; ctx <= CONTEXT_END; ++ctx) {
        for (int g = 0; g < NUM_GROUPS; ++g) {
            if (g != ctx) {
                interWeights[ctx][g] = 0.2;
            }
        }
    }
    
    // 6. Self-model → семантические (для интроспекции)
    for (int sm = SELF_MODEL_START; sm <= SELF_MODEL_END; ++sm) {
        for (int sem = SEMANTIC_START; sem <= SEMANTIC_END; ++sem) {
            interWeights[sm][sem] = 0.15;
        }
    }
}

// ============================================================================
// ОСНОВНОЙ STEP
// ============================================================================

void NeuralFieldSystem::step(float external_reward, int stepNumber) {
    stepCounter = stepNumber;
    
    // ===== ФАЗА 1: Эволюция всех групп =====
    for (auto& g : groups) {
        g.evolve();
    }
    
    // ===== ФАЗА 2: EmergentController — расчёт surprise, quality =====
    {
        auto avgs_d = getGroupAverages();
        std::vector<float> avgs_f(avgs_d.begin(), avgs_d.end());
        lastSignal_ = emergent_.tick(avgs_f, groups, external_reward, stepNumber);
    }
    
    // Обновление температуры внимания
    {
        float new_temp = attention.temperature + lastSignal_.temperature_delta;
        if (lastSignal_.surprise > 0.9f) {
            new_temp = std::min(new_temp, 2.0f);
        }
        attention.temperature = std::clamp(new_temp, 0.1f, 5.0f);
    }
    
    // ===== ФАЗА 3: Self-model — сэмплирование и инъекция =====
    {
        auto snap = self_sampler_.sample(*this, lastSignal_, stepNumber);
        self_sampler_.inject(*this, snap);
    }
    
    // ===== ФАЗА 4: Межгрупповое взаимодействие (простая рекуррентная динамика) =====
    {
        auto currAvg = getGroupAverages();
        std::vector<double> newAvg(NUM_GROUPS, 0.0);
        
        // Простая рекуррентная динамика с межгрупповыми связями
        for (int g = 0; g < NUM_GROUPS; ++g) {
            double input = 0.0;
            for (int h = 0; h < NUM_GROUPS; ++h) {
                if (h != g) {
                    input += interWeights[g][h] * currAvg[h];
                }
            }
            // Также учитываем внешние входы
            if (g == INPUT_GROUP && !external_inputs_.empty()) {
                for (size_t i = 0; i < std::min(external_inputs_.size(), (size_t)GROUP_SIZE); ++i) {
                    input += external_inputs_[i] * 0.1;
                }
            }
            newAvg[g] = std::clamp(currAvg[g] + dt_ * input, 0.0, 1.0);
        }
        
        // Применяем новые активности к группам
        for (int g = 0; g < NUM_GROUPS; ++g) {
            double diff = newAvg[g] - groups[g].getAverageActivity();
            auto& phi = groups[g].getPhiNonConst();
            for (int n = 0; n < GROUP_SIZE; ++n) {
                phi[n] = std::clamp(phi[n] + dt_ * diff * 0.1, 0.0, 1.0);
            }
        }
    }

    // ===== НОВАЯ ФАЗА 4.5: LAGRANGIAN АУДИТ =====
    if (energy_audit_enabled_) {
        // Сохраняем предыдущее состояние
        previous_canonical_state_ = canonical_state_;
        
        // Получаем текущее каноническое состояние
        canonical_state_ = lagrangian_auditor_.toCanonical(groups, interWeights, dt_);
        
        // Аудируем сохранение энергии
        bool energy_conserved = lagrangian_auditor_.auditEnergyConservation(canonical_state_, interWeights, dt_);
        
        if (!energy_conserved && lagrangian_auditor_.getConfig().auto_correct) {
            // Корректируем состояние
            lagrangian_auditor_.correctState(canonical_state_, 0.0, interWeights);
            
            // Применяем коррекцию к группам
            for (int g = 0; g < NUM_GROUPS; ++g) {
                double target_q = canonical_state_.q[g];
                double current_q = groups[g].getAverageActivity();
                double delta = target_q - current_q;
                
                auto& phi = groups[g].getPhiNonConst();
                for (int n = 0; n < GROUP_SIZE; ++n) {
                    phi[n] = std::clamp(phi[n] + delta * 0.1, 0.0, 1.0);
                }
            }
        }
        
        // Обновляем lastSignal_ с риском на основе энергии
        float energy_risk = static_cast<float>(lagrangian_auditor_.getEnergyError());
        lastSignal_.hallucination_risk = std::max(lastSignal_.hallucination_risk, energy_risk);
    }
    
    // ===== ФАЗА 5: Распределение наград =====
    routeRewards(lastSignal_, stepNumber);
    

    
    // ===== ФАЗА 7: Консолидация =====
    bool force_consolidate = (stepNumber % MIN_CONSOLIDATION_INTERVAL == 0);
    if (lastSignal_.should_consolidate || force_consolidate) {
        float importance = lastSignal_.consolidation_pressure;
        if (importance < 0.1f && force_consolidate) importance = 0.2f;
        
        for (auto& g : groups) {
            g.consolidateEligibility(importance);
            g.consolidateElevation(importance);
        }
        consolidateInterWeights(importance);
        applyPruningByElevation();
        
        entropy_history.push_back(computeSystemEntropy());
        if (entropy_history.size() > HISTORY_SIZE) entropy_history.pop_front();
    }
    
    // ===== ФАЗА 8: Латеральное торможение =====
    applyLateralInhibition();
    
    // ===== ФАЗА 9: Обновление elevation =====
    for (int g = 0; g < NUM_GROUPS; ++g) {
        groups[g].updateElevationFast(lastSignal_.quality, groups[g].getAverageActivity());
    }
    
    // ===== ФАЗА 10: Диагностика =====
    if (stepNumber % 1000 == 0) {
        logSystemHealth();
        diagnoseCriticality();
        std::cout << "[Emergent] STM=" << lastSignal_.stm_size
                  << " LTM=" << lastSignal_.ltm_size
                  << " surprise=" << std::fixed << std::setprecision(3) << lastSignal_.surprise
                  << " quality=" << lastSignal_.quality
                  << " trend=" << lastSignal_.improvement_trend
                  << " temp=" << attention.temperature
                  << std::endl;
    }
    
    // ===== ФАЗА 11: Эволюционный запрос (редко) =====
    if (stepNumber % EVOLUTION_INTERVAL == 0) {
        pendingEvolution_ = true;
    }
    
    flatDirty = true;
}

// ============================================================================
// РАСПРЕДЕЛЕНИЕ НАГРАД
// ============================================================================

void NeuralFieldSystem::routeRewards(const EmergentSignal& sig, int step) {
    constexpr float SURVIVAL = 0.02f;
    
    for (int g = 0; g < NUM_GROUPS; ++g) {
        float pred_reward = (g < (int)sig.per_group_reward.size()) 
                            ? sig.per_group_reward[g] 
                            : 0.5f;
        
        float reward;
        
        // Разные правила для разных типов групп
        if (g == INPUT_GROUP) {
            reward = SURVIVAL + 0.08f * pred_reward;
        } 
        else if (g >= SEMANTIC_START && g <= SEMANTIC_END) {
            reward = pred_reward * 0.6f + sig.quality * 0.4f;
        }
        else if (g >= MOTOR_START && g <= MOTOR_END) {
            reward = pred_reward * 0.5f + sig.quality * 0.3f;
        }
        else if (g >= SELF_MODEL_START && g <= SELF_MODEL_END) {
            reward = pred_reward * 0.7f + sig.quality * 0.3f;
        }
        else {
            reward = pred_reward * 0.4f + SURVIVAL;
        }
        
        // Exploration bonus
        if (sig.should_explore) {
            reward += 0.05f * sig.surprise;
        }
        
        reward = std::clamp(reward, 0.f, 1.f);
        groups[g].learnSTDP(reward, step);
    }
}

// ============================================================================
// КОНСОЛИДАЦИЯ МЕЖГРУППОВЫХ СВЯЗЕЙ
// ============================================================================

void NeuralFieldSystem::consolidateInterWeights(float pressure) {
    double entropy = getUnifiedEntropy();
    double entropy_factor = 1.0 / (1.0 + std::exp(-(entropy - 0.5) * 4.0));
    
    double scale = 0.999 + 0.001 * entropy_factor;
    double boost = 1.0 + static_cast<double>(pressure) * 0.01;
    
    for (auto& row : interWeights) {
        for (auto& w : row) {
            w = std::clamp(w * scale * boost, -0.5, 0.5);
        }
    }
}

// ============================================================================
// ЛАТЕРАЛЬНОЕ ТОРМОЖЕНИЕ (СЕМАНТИЧЕСКИЕ ГРУППЫ)
// ============================================================================

void NeuralFieldSystem::applyLateralInhibition() {
    const int NUM_SEM = SEMANTIC_END - SEMANTIC_START + 1;
    std::vector<double> act(NUM_GROUPS, 0.0);
    
    for (int g = 0; g < NUM_GROUPS; ++g) {
        const auto& phi = groups[g].getPhi();
        act[g] = std::accumulate(phi.begin(), phi.end(), 0.0) / GROUP_SIZE;
    }
    
    const double INHIB = 0.1;
    for (int g = SEMANTIC_START; g <= SEMANTIC_END; ++g) {
        double inh = 0.0;
        for (int o = SEMANTIC_START; o <= SEMANTIC_END; ++o) {
            if (o != g) inh += act[o];
        }
        auto& phi = groups[g].getPhiNonConst();
        for (int i = 0; i < GROUP_SIZE; ++i) {
            phi[i] = std::max(0.0, phi[i] - INHIB * inh);
        }
    }
}

// ============================================================================
// ПРУНИНГ ПО ELEVATION
// ============================================================================

void NeuralFieldSystem::applyPruningByElevation() {
    for (auto& g : groups) {
        if (g.getElevation() < -0.5f) {
            g.decayAllWeights(0.95f);
        }
    }
}

// ============================================================================
// ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================

void NeuralFieldSystem::rebuildFlatVectors() const {
    if (!flatDirty) return;
    int idx = 0;
    for (const auto& grp : groups) {
        const auto& phiGrp = grp.getPhi();
        const auto& piGrp = grp.getPi();
        for (int i = 0; i < GROUP_SIZE; ++i) {
            flatPhi[idx] = phiGrp[i];
            flatPi[idx] = piGrp[i];
            ++idx;
        }
    }
    flatDirty = false;
}

double NeuralFieldSystem::computeSystemEntropy() const {
    rebuildFlatVectors();
    const int BINS = 20;
    std::vector<int> hist(BINS, 0);
    for (double v : flatPhi) {
        int bin = std::clamp(static_cast<int>(v * BINS), 0, BINS - 1);
        hist[bin]++;
    }
    double entropy = 0.0;
    double total = static_cast<double>(TOTAL_NEURONS);
    for (int count : hist) {
        if (count > 0) {
            double p = static_cast<double>(count) / total;
            entropy -= p * std::log(p);
        }
    }
    return entropy;
}

double NeuralFieldSystem::getUnifiedEntropy() const {
    rebuildFlatVectors();
    const int BINS = 32;
    std::vector<int> hist(BINS, 0);
    for (double v : flatPhi) {
        int bin = std::clamp(static_cast<int>(v * BINS), 0, BINS - 1);
        hist[bin]++;
    }
    double H = 0.0, total = static_cast<double>(TOTAL_NEURONS);
    for (int count : hist) {
        if (count > 0) {
            double p = count / total;
            H -= p * std::log2(p);
        }
    }
    return std::clamp(H / std::log2(static_cast<double>(BINS)), 0.0, 1.0);
}

double NeuralFieldSystem::getTargetUnifiedEntropy() const {
    double base = 0.5;
    // Убираем зависимость от pred_error, используем энтропию как прокси
    double entropy = getUnifiedEntropy();
    double err_factor = 1.0 + std::min(0.5, std::abs(entropy - 0.5) * 2.0);
    double mode_factor = 1.0;
    switch (current_mode_) {
        case OperatingMode::TRAINING: mode_factor = 1.3; break;
        case OperatingMode::IDLE:     mode_factor = 0.7; break;
        case OperatingMode::SLEEP:    mode_factor = 1.2; break;
        default: break;
    }
    return std::clamp(base * err_factor * mode_factor, 0.2, 0.8);
}

std::vector<double> NeuralFieldSystem::getGroupAverages() const {
    std::vector<double> avgs(NUM_GROUPS);
    for (int g = 0; g < NUM_GROUPS; ++g) {
        avgs[g] = groups[g].getAverageActivity();
    }
    return avgs;
}

void NeuralFieldSystem::strengthenInterConnection(int from, int to, double delta) {
    if (from >= 0 && from < NUM_GROUPS && to >= 0 && to < NUM_GROUPS && from != to) {
        interWeights[from][to] = std::clamp(interWeights[from][to] + delta, -0.5, 0.5);
    }
}

void NeuralFieldSystem::weakenInterConnection(int from, int to, double delta) {
    strengthenInterConnection(from, to, -delta);
}

std::vector<float> NeuralFieldSystem::getFeatures() const {
    std::vector<float> features(64, 0.f);
    auto avgs = getGroupAverages();
    for (int g = 0; g < NUM_GROUPS; ++g) {
        features[g] = static_cast<float>(avgs[g]);
    }
    for (int g = 0; g < NUM_GROUPS; ++g) {
        const auto& phi = groups[g].getPhi();
        const int BINS = 10;
        std::vector<int> hist(BINS, 0);
        for (double v : phi) {
            hist[std::clamp((int)(v * BINS), 0, BINS - 1)]++;
        }
        double H = 0.0;
        for (int c : hist) {
            if (c > 0) {
                double p = c / (double)GROUP_SIZE;
                H -= p * std::log(p);
            }
        }
        features[NUM_GROUPS + g] = static_cast<float>(H);
    }
    return features;
}

void NeuralFieldSystem::setOperatingMode(OperatingMode::Type mode) {
    current_mode_ = mode;
    for (auto& g : groups) {
        g.setCurrentMode(mode);
    }
}

void NeuralFieldSystem::diagnoseCriticality() {
    double H = computeSystemEntropy();
    static double last_H = H;
    double dH = std::abs(H - last_H);
    if (dH > 0.1) {
        std::cout << "⚠️ Phase transition: ΔH=" << dH << std::endl;
    }
    if (H < 0.5) {
        std::cout << "❌ Frozen. Boosting temp." << std::endl;
        attention.temperature *= 1.5f;
    }
    if (H > 3.0) {
        std::cout << "❌ Chaotic. Reducing temp." << std::endl;
        attention.temperature *= 0.7f;
    }
    last_H = H;
}

void NeuralFieldSystem::logSystemHealth() {
    if (stepCounter % 1000 != 0) return;
    
    std::cout << "\n=== SYSTEM HEALTH ===\n";
    
    // Статистика по типам групп
    std::map<std::string, double> avg_activity;
    std::map<std::string, int> counts;
    
    for (int g = 0; g < NUM_GROUPS; ++g) {
        std::string spec = groups[g].getSpecialization();
        avg_activity[spec] += groups[g].getAverageActivity();
        counts[spec]++;
    }
    
    // Старый синтаксис без structured binding
    for (std::map<std::string, double>::iterator it = avg_activity.begin(); 
         it != avg_activity.end(); ++it) {
        const std::string& spec = it->first;
        double total = it->second;
        total /= counts[spec];
        std::cout << spec << ": " << std::fixed << std::setprecision(3) << total << std::endl;
    }
    
    std::cout << "Temperature: " << attention.temperature << std::endl;
    std::cout << "Entropy: " << getUnifiedEntropy() << std::endl;
    std::cout << "====================\n";
}

double NeuralFieldSystem::computeOptimalStructure() {
    double temperature = attention.temperature;
    std::vector<double> probs;
    double Z = 0.0;
    for (const auto& g : groups) {
        double E = g.getAverageActivity();
        double arg = std::clamp(-E / (temperature + 1e-6), -50.0, 50.0);
        double p = std::exp(arg);
        probs.push_back(p);
        Z += p;
    }
    if (Z < 1e-12) return 0.0;
    double H = 0.0;
    for (double p : probs) {
        p /= Z;
        if (p > 1e-12) H -= p * std::log2(p);
    }
    return H;
}

void NeuralFieldSystem::applyResourcePenalty(float penalty) {
    for (auto& g : groups) {
        g.applyGlobalPenalty(penalty);
    }
    if (penalty < -0.3f) {
        attention.temperature = std::clamp(attention.temperature * 0.98f, 0.1f, 5.0f);
    }
}

void NeuralFieldSystem::enterLowPowerMode() {
    for (auto& g : groups) {
        if (g.getAverageActivity() > 0.3) {
            auto& phi = g.getPhiNonConst();
            for (auto& v : phi) {
                v = std::max(v * 0.7, 0.2);
            }
        }
    }
}

void NeuralFieldSystem::applyTargetPattern(const std::vector<float>& pat) {
    for (int g = 0; g < 6 && g < (int)pat.size() / GROUP_SIZE; ++g) {
        auto& phi = groups[SEMANTIC_START + g].getPhiNonConst();
        for (int i = 0; i < GROUP_SIZE; ++i) {
            float diff = pat[g * GROUP_SIZE + i] - static_cast<float>(phi[i]);
            phi[i] = std::clamp(phi[i] + diff * 0.1, 0.0, 1.0);
        }
    }
}

void NeuralFieldSystem::applyTargetedMutation(double strength, int targetType) {
    std::mt19937 gen(std::random_device{}());
    std::uniform_real_distribution<double> d(-strength, strength);
    
    if (targetType == 0) {
        for (int i = 0; i < NUM_GROUPS; ++i) {
            for (int j = 0; j < NUM_GROUPS; ++j) {
                if (i != j) {
                    interWeights[i][j] += d(gen) * 0.1;
                    interWeights[i][j] = std::clamp(interWeights[i][j], -0.5, 0.5);
                }
            }
        }
    } else {
        std::uniform_int_distribution<> gi(0, NUM_GROUPS - 1);
        int g = gi(gen);
        groups[g].setStdpRate(groups[g].getStdpRate() + d(gen) * 0.01f);
    }
}

void NeuralFieldSystem::setInputText(const std::vector<float>& sig) {
    auto& phi = groups[INPUT_GROUP].getPhiNonConst();
    for (int i = 0; i < GROUP_SIZE && i < (int)sig.size(); ++i) {
        phi[i] = sig[i];
    }
    for (int t = SEMANTIC_START; t <= SEMANTIC_END; ++t) {
        strengthenInterConnection(INPUT_GROUP, t, 0.1);
    }
}

// ============================================================================
// РЕФЛЕКСИЯ (заглушки)
// ============================================================================

NeuralFieldSystem::ReflectionState NeuralFieldSystem::getReflectionState() const {
    ReflectionState s;
    s.confidence = lastSignal_.quality;
    s.curiosity = lastSignal_.surprise;
    s.satisfaction = lastSignal_.improvement_trend > 0 ? 0.7 : 0.3;
    s.confusion = lastSignal_.should_explore ? 0.7 : 0.2;
    s.attention_map.assign(4, 0.25);
    return s;
}

void NeuralFieldSystem::reflect() {}
void NeuralFieldSystem::setGoal(const std::string& g) { current_goal = g; }
bool NeuralFieldSystem::evaluateProgress() { return lastSignal_.improvement_trend > 0; }
void NeuralFieldSystem::learnFromReflection(float) {}