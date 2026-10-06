#include "NeuralGroup.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <numeric>
#include <random>
#include <deque>

// ============================================================================
// КОНСТРУКТОР
// ============================================================================

NeuralGroup::NeuralGroup(int size, double dt, std::shared_ptr<std::mt19937> rng)
    : size_(size), dt_(dt), rng_(std::move(rng))
{
    if (!rng_) {
        throw std::runtime_error("NeuralGroup: RNG is null");
    }
    // Инициализация мембранных полей
    V_.resize(size_, membrane_params_.v_rest);
    V_threshold_.resize(size_, membrane_params_.v_threshold_base);
    spike_.resize(size_, false);
    last_spike_step_.resize(size_, -1000);
    refractory_.resize(size_, 0);
    spike_history_.resize(size_);
    
    // Инициализация трофических полей
    trophic_signal_.resize(size_, 0.0);
    trophic_accumulator_.resize(size_, 0.1);  // начинаем с маленького, чтобы не умирали сразу
    
    // Инициализация апоптоза и критического периода
    apoptosis_timer_.resize(size_, 0);
    critical_period_remaining_.resize(size_, 0);
    plasticity_boost_.resize(size_, 1.0f);
    low_rate_timer_.resize(size_, 0);
    
    // Инициализация весов (слабые случайные)
    W_.assign(size_, std::vector<double>(size_, 0.0));
    std::uniform_real_distribution<double> weight_dist(-0.1, 0.1);
    for (int i = 0; i < size_; ++i) {
        for (int j = i + 1; j < size_; ++j) {
            double w = weight_dist(*rng_);
            W_[i][j] = w;
            W_[j][i] = w;
        }
    }
    
    buildSynapsesFromWeights();
}

// ============================================================================
// ОСНОВНАЯ ЭВОЛЮЦИЯ
// ============================================================================

void NeuralGroup::evolve() {
    step_counter_++;
    
    // 1. Обновление мембранных потенциалов и спайков
    updateMembranePotentials();
    
    // 1.5 Сохранение энергии (Lagrangian constraint)
    enforceEnergyConservation();
    
    // 2. Трофические сигналы (раз в 10 шагов для эффективности)
    if (step_counter_ % 10 == 0) {
        updateTrophicSignals();
    }
    
    // 3. Проверка апоптоза
    checkApoptosis();
    
    // 4. Затухание накопленных трофинов
    for (int i = 0; i < size_; ++i) {
        trophic_accumulator_[i] *= neuro_params_.decay_rate;
        trophic_accumulator_[i] = std::max(0.0, trophic_accumulator_[i]);
    }
    
    // 5. Уменьшение критического периода
    for (int i = 0; i < size_; ++i) {
        if (critical_period_remaining_[i] > 0) {
            critical_period_remaining_[i]--;
            if (critical_period_remaining_[i] == 0) {
                plasticity_boost_[i] = 1.0f;
            }
        }
    }
    
    cache_dirty_ = true;
}

// ----------------------------------------------------------------------------
// Мембранная динамика (LIF)
// ----------------------------------------------------------------------------

void NeuralGroup::updateMembranePotentials() {
    // Оптимизация: сбор активных спайков
    std::vector<int> active_spikes;
    active_spikes.reserve(size_);
    for (int j = 0; j < size_; ++j) {
        if (spike_[j] && step_counter_ - last_spike_step_[j] == 1) {
            active_spikes.push_back(j);
        }
    }
    
    for (int i = 0; i < size_; ++i) {
        if (refractory_[i] > 0) {
            refractory_[i]--;
            spike_[i] = false;
            continue;
        }
        
        double I_syn = 0.0;
        for (int j : active_spikes) {
            I_syn += W_[j][i];
        }
        
        double I_leak = -membrane_params_.g_leak * (V_[i] - membrane_params_.v_rest);
        V_[i] += dt_ * (I_syn + I_leak) / membrane_params_.c_m;
        
        if (V_[i] > V_threshold_[i]) {
            spike_[i] = true;
            last_spike_step_[i] = step_counter_;
            refractory_[i] = membrane_params_.refractory_period;
            V_[i] = membrane_params_.v_reset;
            V_threshold_[i] += membrane_params_.spike_adaptation;
            V_threshold_[i] = std::min(V_threshold_[i], membrane_params_.v_threshold_max);
        } else {
            spike_[i] = false;
            V_threshold_[i] = V_threshold_[i] * membrane_params_.threshold_decay 
                            + membrane_params_.v_threshold_base * (1.0 - membrane_params_.threshold_decay);
        }
        
        spike_history_[i].push_back(spike_[i]);
        if (spike_history_[i].size() > SPIKE_HISTORY_WINDOW) {
            spike_history_[i].pop_front();
        }
    }
}

// ----------------------------------------------------------------------------
// Трофические сигналы (нейротрофины)
// ----------------------------------------------------------------------------

void NeuralGroup::updateTrophicSignals() {
    // Сначала обнуляем входящие трофины
    std::fill(trophic_signal_.begin(), trophic_signal_.end(), 0.0);
    
    // Каждый нейрон выделяет трофин пропорционально своей активности и важности
    for (int i = 0; i < size_; ++i) {
        double trophic_out = computeTrophicOutput(i);
        
        // Передаём трофин постсинаптическим нейронам
        for (int j = 0; j < size_; ++j) {
            if (i != j && std::abs(W_[i][j]) > 0.01) {
                trophic_signal_[j] += trophic_out * std::abs(W_[i][j]) * 0.1;
            }
        }
    }
    
    // Обновляем накопленные трофины
    for (int i = 0; i < size_; ++i) {
        trophic_accumulator_[i] += trophic_signal_[i] * neuro_params_.base_production;
        trophic_accumulator_[i] = std::min(trophic_accumulator_[i], 10.0);  // ограничение
    }
}

double NeuralGroup::computeTrophicOutput(int i) const {
    // Нейрон выделяет трофин, если он:
    // 1. Активен (были спайки)
    // 2. Имеет высокую предсказательную ценность (через memory_manager)
    
    double activity_factor = getFiringRateImpl(i) / 100.0;  // нормализация
    activity_factor = std::min(1.0, activity_factor);
    
    double importance_factor = 0.5;
    if (memory_manager_) {
        // Можно запросить важность этого нейрона из памяти
        // Пока заглушка
        importance_factor = 0.5 + trophic_accumulator_[i] / 10.0;
        importance_factor = std::min(1.0, importance_factor);
    }
    
    return activity_factor * importance_factor;
}

// ----------------------------------------------------------------------------
// Апоптоз и нейрогенез
// ----------------------------------------------------------------------------

void NeuralGroup::checkApoptosis() {
    for (int i = 0; i < size_; ++i) {
        bool should_die = false;
        
        // Условие 1: слишком долго без трофинов
        if (trophic_accumulator_[i] < neuro_params_.accumulation_threshold) {
            apoptosis_timer_[i]++;
            if (apoptosis_timer_[i] > neuro_params_.apoptosis_delay) {
                should_die = true;
            }
        } else {
            apoptosis_timer_[i] = 0;
        }
        
        // Условие 2: слишком низкая частота спайков (менее 0.1 Гц) долгое время
        // отслеживает низкую активность
        if (!should_die && step_counter_ > 10000) {
            double rate = getFiringRateImpl(i);
            if (rate < 0.1 && trophic_accumulator_[i] < 0.1) {
                low_rate_timer_[i]++;
                if (low_rate_timer_[i] > 10000) {
                    should_die = true;
                }
            } else {
                low_rate_timer_[i] = 0;
            }
        }
        
        if (should_die) {
            // Сохраняем "завещание"
            PatternWill will;
            will.importance = trophic_accumulator_[i];
            will.incoming.resize(size_);
            will.outgoing.resize(size_);
            for (int j = 0; j < size_; ++j) {
                will.incoming[j] = W_[j][i];
                will.outgoing[j] = W_[i][j];
            }
            will_pool_.push_back(will);
            if (will_pool_.size() > MAX_WILL_POOL) will_pool_.pop_front();
            
            // Нейрогенез
            neurogenesis(i);
            
            if (step_counter_ % 1000 == 0) {
                std::cout << "[Apoptosis] Neuron " << i << " died, importance=" << will.importance << std::endl;
            }
        }
    }
}

void NeuralGroup::neurogenesis(int i) {
    // Сброс мембранных параметров
    V_[i] = membrane_params_.v_rest;
    V_threshold_[i] = membrane_params_.v_threshold_base;
    spike_[i] = false;
    refractory_[i] = 0;
    trophic_accumulator_[i] = 0.5;  // небольшой запас, чтобы не умер сразу
    
    // Наследование паттерна
    inheritBestPattern(i);
    
    // Критический период
    critical_period_remaining_[i] = neuro_params_.critical_period_length;
    plasticity_boost_[i] = neuro_params_.plasticity_boost;
    
    // Небольшая случайная мутация оставшихся связей
    static std::uniform_real_distribution<double> mutation_dist(-0.05, 0.05);
    for (int j = 0; j < size_; ++j) {
        if (i != j) {
            W_[i][j] += mutation_dist(*rng_);
            W_[j][i] = W_[i][j];
            W_[i][j] = std::clamp(W_[i][j], -1.0, 1.0);
            W_[j][i] = W_[i][j];
        }
    }
    
    syncWeightsFromSynapses();
}

void NeuralGroup::inheritBestPattern(int i) {
    if (will_pool_.empty()) {
        // Если нет завещаний — случайная инициализация
        static std::uniform_real_distribution<double> init_dist(-0.2, 0.2);
        for (int j = 0; j < size_; ++j) {
            if (i != j) {
                W_[i][j] = init_dist(*rng_);
                W_[j][i] = W_[i][j];
            }
        }
        return;
    }
    
    // Выбираем лучшего кандидата по важности
    auto best = std::max_element(will_pool_.begin(), will_pool_.end(),
        [](const PatternWill& a, const PatternWill& b) {
            return a.importance < b.importance;
        });
    
    // Наследование: 70% от лучшего, 30% случайная мутация
    static std::uniform_real_distribution<double> mutation_dist(-0.05, 0.05);
    for (int j = 0; j < size_; ++j) {
        if (i != j) {
            double inherited = best->outgoing[j] * 0.7;
            double mutation = mutation_dist(*rng_) * 0.3;
            W_[i][j] = std::clamp(inherited + mutation, -1.0, 1.0);
            W_[j][i] = W_[i][j];
        }
    }
}

// ----------------------------------------------------------------------------
// STDP ОБУЧЕНИЕ (реальные спайки)
// ----------------------------------------------------------------------------

void NeuralGroup::learnSTDP(float reward, int currentStep) {
    // Использование LUT для оптимизации
    static const auto exp_plus = getExpPlus();
    static const auto exp_minus = []() {
        std::array<float, 21> arr{};
        for (int dt = 0; dt <= 20; ++dt) {
            arr[dt] = std::exp(-static_cast<float>(dt) / 20.0f);
        }
        return arr;
    }();
    
    int synIndex = 0;
    
    for (int i = 0; i < size_; ++i) {
        for (int j = i + 1; j < size_; ++j) {
            if (synIndex >= static_cast<int>(synapses_.size())) break;
            
            auto& syn = synapses_[synIndex++];
            
            if (spike_[i]) syn.lastPreFire = static_cast<float>(currentStep);
            if (spike_[j]) syn.lastPostFire = static_cast<float>(currentStep);
            
            float dt = syn.lastPostFire - syn.lastPreFire;
            float delta = 0.0f;
            
            int dt_int = static_cast<int>(std::abs(dt));
            if (dt > 0 && dt_int <= 20) {
                delta = params_.A_plus * exp_plus[dt_int];
            } else if (dt < 0 && dt_int <= 20) {
                delta = -params_.A_minus * exp_minus[dt_int];
            }
            
            float reward_factor = std::min(1.0f, reward);
            float boost = 1.0f;
            if (critical_period_remaining_[i] > 0) boost *= plasticity_boost_[i];
            if (critical_period_remaining_[j] > 0) boost *= plasticity_boost_[j];
            
            float weight_change = params_.stdpRate * reward_factor * delta * boost;
            
            syn.eligibility = syn.eligibility * params_.eligibilityDecay + delta;
            syn.eligibility = std::clamp(syn.eligibility, -0.1f, 0.1f);
            
            syn.weight += weight_change;
            syn.weight = std::clamp(syn.weight, -params_.maxWeight, params_.maxWeight);
            
            W_[i][j] = syn.weight;
            W_[j][i] = syn.weight;
        }
    }
    
    if (step_counter_ % 100 == 0) {
        for (int i = 0; i < size_; ++i) {
            for (int j = i + 1; j < size_; ++j) {
                if (std::abs(W_[i][j]) < 0.01f) {
                    W_[i][j] *= 0.99f;
                    W_[j][i] = W_[i][j];
                }
            }
        }
    }
}

// ----------------------------------------------------------------------------
// КОНСОЛИДАЦИЯ
// ----------------------------------------------------------------------------

void NeuralGroup::consolidate() {
    for (auto& syn : synapses_) {
        syn.weight += params_.consolidationRate * syn.eligibility;
        syn.eligibility *= 0.9f;
        syn.weight = std::clamp(syn.weight, -params_.maxWeight, params_.maxWeight);
    }
    syncWeightsFromSynapses();
}

void NeuralGroup::consolidateEligibility(float globalImportance) {
    // Аналогично consolidate, но с фактором важности
    for (auto& syn : synapses_) {
        syn.weight += params_.consolidationRate * globalImportance * syn.eligibility;
        syn.eligibility *= 0.5f;
        syn.weight = std::clamp(syn.weight, -params_.maxWeight, params_.maxWeight);
    }
    syncWeightsFromSynapses();
}

// ----------------------------------------------------------------------------
// ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ----------------------------------------------------------------------------

double NeuralGroup::getFiringRate(int i) const {
    return getFiringRateImpl(i);
}

double NeuralGroup::getFiringRateImpl(int i) const {
    if (i < 0 || i >= size_) return 0.0;
    if (spike_history_[i].empty()) return 0.0;
    int spike_count = 0;
    for (bool s : spike_history_[i]) if (s) spike_count++;
    return static_cast<double>(spike_count) / SPIKE_HISTORY_WINDOW;
}

double NeuralGroup::getAverageActivity() const {
    double sum = 0.0;
    for (int i = 0; i < size_; ++i) {
        sum += getFiringRateImpl(i);
    }
    return sum / size_;
}

const std::vector<double>& NeuralGroup::getPhi() const {
    updateCache();
    return phi_cache_;
}

std::vector<double>& NeuralGroup::getPhiNonConst() {
    updateCache();
    return phi_cache_;
}

const std::vector<double>& NeuralGroup::getPi() const {
    updateCache();
    return pi_cache_;
}

std::vector<double>& NeuralGroup::getPiNonConst() {
    updateCache();
    return pi_cache_;
}

void NeuralGroup::updateCache() const {
    if (!cache_dirty_) return;
    phi_cache_.resize(size_);
    pi_cache_.resize(size_);
    for (int i = 0; i < size_; ++i) {
        phi_cache_[i] = getFiringRateImpl(i);
        pi_cache_[i] = spike_[i] ? 1.0 : 0.0;
    }
    cache_dirty_ = false;
}

void NeuralGroup::setWeight(int i, int j, double w) {
    if (i >= 0 && i < size_ && j >= 0 && j < size_) {
        W_[i][j] = std::clamp(w, -1.0, 1.0);
        W_[j][i] = W_[i][j];
        int idx = getSynapseIndex(i, j);
        if (idx >= 0 && idx < (int)synapses_.size()) {
            synapses_[idx].weight = W_[i][j];
        }
    }
}

double NeuralGroup::getWeight(int i, int j) const {
    if (i >= 0 && i < size_ && j >= 0 && j < size_) return W_[i][j];
    return 0.0;
}

void NeuralGroup::decayAllWeights(float factor) {
    for (auto& syn : synapses_) {
        syn.weight *= factor;
    }
    syncWeightsFromSynapses();
}

void NeuralGroup::updateElevationFast(float reward, float activity) {
    elevation_ += elevation_learning_rate_ * reward * activity;
    elevation_ *= elevation_decay_;
    elevation_ = std::clamp(elevation_, -1.0f, 1.0f);
}

void NeuralGroup::consolidateElevation(float globalImportance, float hallucinationPenalty) {
    if (activity_counter_ > 0) {
        float avg_importance = cumulative_importance_ / activity_counter_;
        elevation_ += (avg_importance - 0.5f) * 0.1f;
        elevation_ -= hallucinationPenalty * 0.2f;
        elevation_ = std::clamp(elevation_, -1.0f, 1.0f);
    }
    cumulative_importance_ = 0.0f;
    activity_counter_ = 0;
}

void NeuralGroup::applyGlobalPenalty(float penalty) {
    elevation_ += penalty * 0.01f;
    elevation_ = std::clamp(elevation_, -1.0f, 1.0f);
}

void NeuralGroup::buildSynapsesFromWeights() {
    synapses_.clear();
    for (int i = 0; i < size_; ++i) {
        for (int j = i + 1; j < size_; ++j) {
            Synapse syn;
            syn.weight = static_cast<float>(W_[i][j]);
            synapses_.push_back(syn);
        }
    }
}

void NeuralGroup::syncWeightsFromSynapses() {
    int idx = 0;
    for (int i = 0; i < size_; ++i) {
        for (int j = i + 1; j < size_; ++j) {
            if (idx < (int)synapses_.size()) {
                W_[i][j] = synapses_[idx].weight;
                W_[j][i] = synapses_[idx].weight;
                idx++;
            }
        }
    }
}

int NeuralGroup::getSynapseIndex(int i, int j) const {
    if (i == j) return -1;
    if (i > j) std::swap(i, j);
    return i * size_ - (i * (i + 1)) / 2 + (j - i - 1);
}

void NeuralGroup::logStats() const {
    double avg_weight = 0.0;
    int nonzero = 0;
    for (int i = 0; i < size_; ++i) {
        for (int j = 0; j < size_; ++j) {
            if (i != j && std::abs(W_[i][j]) > 0.01) {
                avg_weight += std::abs(W_[i][j]);
                nonzero++;
            }
        }
    }
    if (nonzero > 0) avg_weight /= nonzero;
    
    int active = 0;
    for (int i = 0; i < size_; ++i) {
        if (getFiringRateImpl(i) > 0.1) active++;
    }
    
    std::cout << "[NeuralGroup " << specialization_ << "] "
              << "Active: " << active << "/" << size_
              << ", Avg weight: " << avg_weight
              << ", Elevation: " << elevation_
              << std::endl;
}