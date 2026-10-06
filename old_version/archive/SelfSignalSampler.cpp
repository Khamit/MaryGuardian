#include "SelfSignalSampler.hpp"
#include "NeuralFieldSystem.hpp"
#include "EmergentCore.hpp"
#include <cmath>
#include <numeric>
#include <algorithm>
#include <iostream>

// ============================================================================
// КОНСТРУКТОР
// ============================================================================

SelfSignalSampler::SelfSignalSampler() {
    reset();
}

void SelfSignalSampler::reset() {
    apoptosis_ema_ = 0.0f;
    neurogenesis_ema_ = 0.0f;
    consolidation_ema_ = 0.0f;
    firing_rate_ema_ = 0.0f;
    
    quality_history_.clear();
    entropy_history_.clear();
    apoptosis_history_.clear();
    neurogenesis_history_.clear();
    
    last_apoptosis_count_ = 0;
    last_neurogenesis_count_ = 0;
    last_ltm_size_ = 0;
    last_avg_firing_rate_ = 0.0f;
    step_counter_ = 0;
}

// ============================================================================
// СБОР СНАПШОТА
// ============================================================================

SelfSnapshot SelfSignalSampler::sample(const NeuralFieldSystem& sys,
                                       const EmergentSignal&    sig,
                                       int                      step) {
    step_counter_ = step;
    SelfSnapshot snap;
    
    const auto& groups = sys.getGroups();
    constexpr int NG = NeuralFieldSystem::NUM_GROUPS;
    constexpr int GS = NeuralFieldSystem::GROUP_SIZE;
    
    // ===== 1. АКТИВНОСТЬ НЕЙРОНОВ (16 сигналов) =====
    
    // Сенсорные группы (1-3)
    float sensory_sum = 0.0f;
    for (int g = NeuralFieldSystem::SENSORY_START; g <= NeuralFieldSystem::SENSORY_END; ++g) {
        sensory_sum += groups[g].getAverageActivity();
    }
    snap.sensory_avg_rate = sensory_sum / 3.0f;
    
    // Моторные группы (4-7)
    float motor_sum = 0.0f;
    for (int g = NeuralFieldSystem::MOTOR_START; g <= NeuralFieldSystem::MOTOR_END; ++g) {
        motor_sum += groups[g].getAverageActivity();
    }
    snap.motor_avg_rate = motor_sum / 4.0f;
    
    // Семантические группы (16-21) — каждая индивидуально
    for (int i = 0; i < 6; ++i) {
        int g = NeuralFieldSystem::SEMANTIC_START + i;
        snap.semantic_rates[i] = groups[g].getAverageActivity();
    }
    
    // Ассоциативные группы (8-15) — объединяем в 4 сигнала
    // 8-11 (первые 4) и 12-15 (следующие 4)
    float assoc_first_sum = 0.0f, assoc_second_sum = 0.0f;
    for (int g = NeuralFieldSystem::ASSOCIATIVE_START; g <= NeuralFieldSystem::ASSOCIATIVE_END; ++g) {
        if (g < NeuralFieldSystem::ASSOCIATIVE_START + 4) {
            assoc_first_sum += groups[g].getAverageActivity();
        } else {
            assoc_second_sum += groups[g].getAverageActivity();
        }
    }
    snap.associative_rates[0] = assoc_first_sum / 4.0f;
    snap.associative_rates[1] = assoc_second_sum / 4.0f;
    snap.associative_rates[2] = (snap.associative_rates[0] + snap.associative_rates[1]) / 2.0f;
    snap.associative_rates[3] = std::abs(snap.associative_rates[0] - snap.associative_rates[1]);
    
    // Контекстные группы (22-27)
    float context_sum = 0.0f;
    for (int g = NeuralFieldSystem::CONTEXT_START; g <= NeuralFieldSystem::CONTEXT_END; ++g) {
        context_sum += groups[g].getAverageActivity();
    }
    snap.context_avg_rate = context_sum / 6.0f;
    
    // Self-model группы (28-31)
    float self_sum = 0.0f;
    for (int g = NeuralFieldSystem::SELF_MODEL_START; g <= NeuralFieldSystem::SELF_MODEL_END; ++g) {
        self_sum += groups[g].getAverageActivity();
    }
    snap.self_model_avg_rate = self_sum / 4.0f;
    
    // ===== 2. ПАМЯТЬ (4 сигнала) =====
    
    const auto& mem = sys.emergent().memory;
    constexpr float STM_CAP = 256.0f;
    constexpr float LTM_CAP = 2048.0f;
    
    snap.stm_fullness = std::clamp(static_cast<float>(mem.stmSize()) / STM_CAP, 0.0f, 1.0f);
    snap.ltm_fullness = std::clamp(static_cast<float>(mem.ltmSize()) / LTM_CAP, 0.0f, 1.0f);
    snap.ltm_avg_importance = mem.averageLTMImportance();
    
    // Скорость консолидации (изменение размера LTM)
    size_t ltm_delta = mem.ltmSize() - last_ltm_size_;
    float consolidation_raw = std::clamp(static_cast<float>(ltm_delta) / 10.0f, -1.0f, 1.0f);
    consolidation_ema_ = computeEma(consolidation_ema_, std::max(0.0f, consolidation_raw));
    snap.memory_consolidation_rate = std::clamp(consolidation_ema_, 0.0f, 1.0f);
    last_ltm_size_ = mem.ltmSize();
    
    // ===== 3. ОБУЧЕНИЕ (4 сигнала) =====
    
    snap.surprise = std::clamp(sig.surprise, 0.0f, 1.0f);
    snap.quality = std::clamp(sig.quality, 0.0f, 1.0f);
    
    // Тренд улучшения качества
    quality_history_.push_back(snap.quality);
    if (quality_history_.size() > HISTORY_MAX) quality_history_.pop_front();
    snap.improvement_trend = std::clamp(computeTrend(quality_history_, TREND_WINDOW), 0.0f, 1.0f);
    
    // Давление к исследованию = surprise + (1 - качество)
    snap.exploration_pressure = std::clamp(snap.surprise + (1.0f - snap.quality), 0.0f, 1.0f);
    
    // ===== 4. ГОМЕОСТАЗ (4 сигнала) =====
    
    snap.system_entropy = static_cast<float>(sys.getUnifiedEntropy());
    float target_entropy = static_cast<float>(sys.getTargetUnifiedEntropy());
    snap.entropy_error = std::clamp(std::abs(snap.system_entropy - target_entropy) * 5.0f, 0.0f, 1.0f);
    snap.attention_temperature = std::clamp(sys.getAttentionTemperature() / 5.0f, 0.0f, 1.0f);
    snap.avg_trophic_level = computeAverageTrophicLevel(sys);
    
    // ===== 5. ДИАГНОСТИКА (4 сигнала) =====
    
    // Скорость апоптоза (смертей нейронов)
    int apoptosis_count = countApoptosisEvents(sys);
    int apoptosis_delta = apoptosis_count - last_apoptosis_count_;
    apoptosis_ema_ = computeEma(apoptosis_ema_, std::clamp(apoptosis_delta / 10.0f, 0.0f, 1.0f));
    snap.apoptosis_rate = apoptosis_ema_;
    last_apoptosis_count_ = apoptosis_count;
    
    // Скорость нейрогенеза (рождений) — аналогично
    int neurogenesis_count = countNeurogenesisEvents(sys);
    int neurogenesis_delta = neurogenesis_count - last_neurogenesis_count_;
    neurogenesis_ema_ = computeEma(neurogenesis_ema_, std::clamp(neurogenesis_delta / 10.0f, 0.0f, 1.0f));
    snap.neurogenesis_rate = neurogenesis_ema_;
    last_neurogenesis_count_ = neurogenesis_count;
    
    // Средняя частота спайков
    snap.avg_firing_rate = computeAverageFiringRate(sys);
    
    // Общая активность сети (доля нейронов с частотой > 0.1)
    snap.network_activity = computeActivityLevel(sys);
    
    // Сохраняем для диагностики
    last_snap_ = snap;
    
    // Логирование раз в 1000 шагов
    if (step % 1000 == 0) {
        std::cout << "[SelfSignal] Quality=" << snap.quality
                  << " Surprise=" << snap.surprise
                  << " Entropy=" << snap.system_entropy
                  << " Apoptosis=" << snap.apoptosis_rate
                  << " FiringRate=" << snap.avg_firing_rate
                  << std::endl;
    }
    
    return snap;
}

// ============================================================================
// ИНЖЕКЦИЯ В SELF-MODEL ГРУППЫ
// ============================================================================

void SelfSignalSampler::inject(NeuralFieldSystem& sys, const SelfSnapshot& snap) {
    auto signals = snap.toVector();
    auto& groups = sys.getGroupsNonConst();
    float alpha = injection_strength;
    
    for (int gi = 0; gi < SELF_GROUP_COUNT; ++gi) {
        int group_idx = SELF_GROUP_START + gi;
        if (group_idx >= NeuralFieldSystem::NUM_GROUPS) break;
        
        auto& phi = groups[group_idx].getPhiNonConst();
        int base = gi * SIGNALS_PER_GROUP;
        
        for (int ni = 0; ni < SIGNALS_PER_GROUP; ++ni) {
            if (base + ni >= (int)signals.size()) break;
            float signal = signals[base + ni];
            // Плавное смешивание с текущей активностью
            phi[ni] = phi[ni] * (1.0f - alpha) + signal * alpha;
            // Ограничиваем в [0, 1] (активность нейрона)
            if (phi[ni] < 0.0) phi[ni] = 0.0;
            if (phi[ni] > 1.0) phi[ni] = 1.0;
        }
    }
}

// ============================================================================
// ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================

float SelfSignalSampler::computeEma(float old_val, float new_val) const {
    return old_val * (1.0f - ema_alpha) + new_val * ema_alpha;
}

float SelfSignalSampler::computeTrend(const std::deque<float>& history, int window) const {
    if (history.size() < (size_t)window * 2) return 0.5f;
    
    int n = (int)history.size();
    float recent = 0.0f, old = 0.0f;
    
    for (int i = n - window; i < n; ++i) recent += history[i];
    for (int i = n - window * 2; i < n - window; ++i) old += history[i];
    
    recent /= window;
    old /= window;
    
    // Нормализуем разницу в [0,1]
    float diff = recent - old;
    return std::clamp(diff * 2.0f + 0.5f, 0.0f, 1.0f);
}

float SelfSignalSampler::computeActivityLevel(const NeuralFieldSystem& sys) const {
    const auto& groups = sys.getGroups();
    constexpr int GS = NeuralFieldSystem::GROUP_SIZE;
    
    int active_count = 0;
    int total_neurons = 0;
    
    for (const auto& group : groups) {
        const auto& phi = group.getPhi();
        for (float activity : phi) {
            if (activity > 0.1f) active_count++;
            total_neurons++;
        }
    }
    
    return total_neurons > 0 ? static_cast<float>(active_count) / total_neurons : 0.0f;
}

float SelfSignalSampler::computeAverageFiringRate(const NeuralFieldSystem& sys) const {
    const auto& groups = sys.getGroups();
    float sum = 0.0f;
    int total = 0;
    
    for (const auto& group : groups) {
        const auto& phi = group.getPhi();
        for (float activity : phi) {
            sum += activity;
            total++;
        }
    }
    
    float current_avg = total > 0 ? sum / total : 0.0f;
    firing_rate_ema_ = computeEma(firing_rate_ema_, current_avg);
    return firing_rate_ema_;
}

float SelfSignalSampler::computeAverageTrophicLevel(const NeuralFieldSystem& sys) const {
    const auto& groups = sys.getGroups();
    float sum = 0.0f;
    int total = 0;
    
    for (const auto& group : groups) {
        for (int i = 0; i < group.getSize(); ++i) {
            sum += group.getTrophicAccumulator(i);
            total++;
        }
    }
    
    // Нормализуем: типичный диапазон 0-10, но мы ограничим 0-1
    float avg = total > 0 ? sum / total : 0.0f;
    return std::clamp(avg / 10.0f, 0.0f, 1.0f);
}

int SelfSignalSampler::countApoptosisEvents(const NeuralFieldSystem& sys) const {
    // В новой архитектуре нет прямого счётчика апоптоза.
    // Используем proxy: количество нейронов с trophic_accumulator < порога
    // или используем ejection_count из NeuralGroup (если он ещё есть)
    const auto& groups = sys.getGroups();
    int low_trophic_count = 0;
    
    for (const auto& group : groups) {
        for (int i = 0; i < group.getSize(); ++i) {
            if (group.getTrophicAccumulator(i) < 0.05f) {
                low_trophic_count++;
            }
        }
    }
    
    // Сглаживаем через EMA
    static float low_trophic_ema = 0.0f;
    low_trophic_ema = computeEma(low_trophic_ema, low_trophic_count / 1024.0f);
    
    return static_cast<int>(low_trophic_ema * 100.0f);
}

int SelfSignalSampler::countNeurogenesisEvents(const NeuralFieldSystem& sys) const {
    // Нейрогенез происходит при апоптозе. Используем proxy:
    // количество нейронов в критическом периоде
    const auto& groups = sys.getGroups();
    int critical_count = 0;
    
    // В новой архитектуре нет прямого доступа к critical_period_remaining.
    // Это заглушка — в реальности нужно добавить геттер в NeuralGroup
    // или использовать другие метрики.
    
    // Временное решение: используем изменение активности как proxy
    static float last_avg_activity = 0.0f;
    float current_avg = computeAverageFiringRate(sys);
    float delta = std::abs(current_avg - last_avg_activity);
    last_avg_activity = current_avg;
    
    return static_cast<int>(delta * 100.0f);
}