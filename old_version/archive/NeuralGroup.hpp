#pragma once

#include <vector>
#include <random>
#include <string>
#include <deque>
#include <cmath>
#include <algorithm>
#include <iostream>
#include "Synapse.hpp"
#include "OperatingMode.hpp"

#include <memory>  // для shared_ptr
#include <array>   // для LUT


#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// разрываем циклическую зависимость
class EmergentMemory;

// ============================================================================
// НОВАЯ АРХИТЕКТУРА
// ============================================================================

/**
 * @struct NeurotrophinParams
 * @brief Параметры трофических сигналов (нейротрофинов)
 */
struct NeurotrophinParams {
    double base_production = 0.1;      // базовая продукция трофина
    double decay_rate = 0.99;          // затухание трофина
    double accumulation_threshold = 0.05; // порог апоптоза
    double apoptosis_delay = 5000;     // шагов без трофинов до смерти
    double critical_period_length = 200; // шагов повышенной пластичности
    double plasticity_boost = 3.0;     // множитель обучения в критический период
};

/**
 * @struct MembraneParams
 * @brief Параметры мембранной динамики (LIF нейрон)
 */
struct MembraneParams {
    double v_rest = -70.0;      // мВ — потенциал покоя
    double v_reset = -80.0;     // мВ — после спайка
    double v_threshold_base = -55.0; // мВ — базовый порог
    double v_threshold_max = -35.0;  // мВ — максимум после адаптации
    double c_m = 1.0;           // мембранная ёмкость
    double g_leak = 0.1;        // проводимость утечки
    int refractory_period = 5;  // шагов рефрактерности
    double spike_adaptation = 5.0; // увеличение порога после спайка (мВ)
    double threshold_decay = 0.999; // возврат порога к базовому

    MembraneParams() = default;
};

/**
 * @class NeuralGroup
 * @brief Группа нейронов с LIF динамикой, STDP, трофическими сигналами и апоптозом
 * 
 * Ключевые принципы:
 * 1. Нейроны имеют мембранный потенциал и генерируют спайки
 * 2. STDP работает на реальных временах спайков
 * 3. Трофические сигналы определяют выживание нейрона
 * 4. Апоптоз + нейрогенез с наследованием весов
 * 5. Критический период для новых нейронов
 */
class NeuralGroup {
public:
    // используем shared_ptr для безопасного владения RNG
    NeuralGroup(int size, double dt, std::shared_ptr<std::mt19937> rng);

    ~NeuralGroup() = default;
    
    // Запрет копирования, разрешаем перемещение
    NeuralGroup(const NeuralGroup&) = delete;
    NeuralGroup& operator=(const NeuralGroup&) = delete;
    NeuralGroup(NeuralGroup&&) = default;
    NeuralGroup& operator=(NeuralGroup&&) = default;
    
    // ===== ОСНОВНЫЕ МЕТОДЫ =====
    void evolve();                                    // один шаг симуляции
    void learnSTDP(float reward, int currentStep);   // STDP обучение
    void consolidate();                               // консолидация памяти
    
    // ===== МЕМБРАННАЯ ДИНАМИКА =====
    void setMembraneParams(const MembraneParams& p) { membrane_params_ = p; }
    double getMembranePotential(int i) const { return i >= 0 && i < size_ ? V_[i] : 0.0; }
    bool getSpike(int i) const { return i >= 0 && i < size_ ? spike_[i] : false; }
    double getFiringRate(int i) const;
    
    // ===== ТРОФИЧЕСКИЕ СИГНАЛЫ =====
    void setNeurotrophinParams(const NeurotrophinParams& p) { neuro_params_ = p; }
    // ===== СИНАПСЫ =====
    void setWeight(int i, int j, double w);
    double getWeight(int i, int j) const;
    const std::vector<std::vector<double>>& getWeights() const { return W_; }
    void decayAllWeights(float factor);
    
    // ===== АКТИВНОСТЬ (для обратной совместимости) =====
    double getAverageActivity() const;
    const std::vector<double>& getPhi() const;
    std::vector<double>& getPhiNonConst();
    const std::vector<double>& getPi() const;
    std::vector<double>& getPiNonConst();

    // ===== NEW DATA =====
    std::vector<std::deque<bool>> spike_history_;
    static constexpr int SPIKE_HISTORY_WINDOW = 100;
    void setCurrentMode(OperatingMode::Type mode) { current_mode_ = mode; }
    OperatingMode::Type getCurrentMode() const { return current_mode_; }
    // Для доступа из EmergentMemory
    double getTrophicAccumulator(int i) const { 
        return i >= 0 && i < size_ ? trophic_accumulator_[i] : 0.0; 
    }
    int getNeuronCount() const { return size_; }
    // Для captureFromNeuron (дружественный доступ)
    const std::vector<std::vector<double>>& getWeightMatrix() const { return W_; }
    // Добавить константу
    static constexpr int MAX_NEURONS = 1024;
    
    // ===== ПАРАМЕТРЫ ОБУЧЕНИЯ =====
    void setLearningRate(double lr) { params_.hebbianRate = lr; }
    double getLearningRate() const { return params_.hebbianRate; }
    void setStdpRate(float rate) { params_.stdpRate = rate; }
    float getStdpRate() const { return params_.stdpRate; }
    void setConsolidationRate(float rate) { params_.consolidationRate = rate; }
    float getConsolidationRate() const { return params_.consolidationRate; }
    
    // ===== ВЫСОТА (ELEVATION) =====
    void setElevation(float elev) { elevation_ = elev; }
    float getElevation() const { return elevation_; }
    void updateElevationFast(float reward, float activity);
    void consolidateElevation(float globalImportance, float hallucinationPenalty = 0.0f);
    void consolidateEligibility(float globalImportance);
    void applyGlobalPenalty(float penalty);
    
    // ===== ПАМЯТЬ =====
    void setMemoryManager(EmergentMemory* mm) { memory_manager_ = mm; }
    void setSpecialization(const std::string& spec) { specialization_ = spec; }
    const std::string& getSpecialization() const { return specialization_; }
    void setSelfModelGroup(bool v) { is_self_model_group_ = v; }
    bool isSelfModelGroup() const { return is_self_model_group_; }
    void setInputGroup(bool v) { is_input_group_ = v; }
    bool isInputGroup() const { return is_input_group_; }
    
    // ===== ДИАГНОСТИКА =====
    int getSize() const { return size_; }
    int getStepCounter() const { return step_counter_; }
    void logStats() const;

        // Вычисление "энергии" состояния нейрона
    double computeNeuralLagrangian(int neuron_idx) const {
        // L = T - V, где T — "кинетическая" (активность), V — "потенциальная" (связи)
        double kinetic = getFiringRate(neuron_idx);  // текущая активность
        
        double potential = 0.0;
        for (int j = 0; j < size_; ++j) {
            potential += std::abs(W_[neuron_idx][j]) * getFiringRate(j);
        }
        
        return kinetic - 0.5 * potential;  // Lagrangian
    }
    
    // Сохранение энергии — ОГРАНИЧЕНИЕ, а не цель
    void enforceEnergyConservation() {
        double current_energy = 0.0;
        for (int i = 0; i < size_; ++i) {
            current_energy += computeNeuralLagrangian(i);
        }
        
        // Если энергия упала — восстановить
        if (current_energy < conserved_energy_ * 0.95) {
            double deficit = conserved_energy_ - current_energy;
            // Распределить дефицит как "толчок"
            for (int i = 0; i < size_; ++i) {
                if (getFiringRate(i) < 0.5) {
                    V_[i] += deficit * 0.01 * dt_;
                }
            }
        }
        conserved_energy_ = conserved_energy_ * 0.999 + current_energy * 0.001;
    }
    
private:
    // ===== ФУНДАМЕНТАЛЬНЫЕ ПАРАМЕТРЫ =====
    int size_;                      // количество нейронов
    double dt_;                     // шаг времени (с)
    int step_counter_ = 0;
    std::string specialization_ = "unspecialized";
    OperatingMode::Type current_mode_ = OperatingMode::NORMAL;
    
    // ===== МЕМБРАННАЯ ДИНАМИКА =====
    std::vector<double> V_;                 // мембранный потенциал (мВ)
    std::vector<double> V_threshold_;       // адаптивный порог спайка (мВ)
    std::vector<bool> spike_;               // спайк на текущем шаге
    std::vector<int> last_spike_step_;      // время последнего спайка
    std::vector<int> refractory_;           // счётчик рефрактерности
    MembraneParams membrane_params_;
    
    // ===== ТРОФИЧЕСКИЕ СИГНАЛЫ =====
    std::vector<double> trophic_signal_;    // получаемый трофин (вход)
    std::vector<double> trophic_accumulator_; // накопленный трофин (скользящее среднее)
    NeurotrophinParams neuro_params_;
    
    // ===== АПОПТОЗ И НЕЙРОГЕНЕЗ =====
    std::vector<int> apoptosis_timer_;      // счётчик до смерти
    std::vector<int> critical_period_remaining_; // шагов до конца критического периода
    std::vector<float> plasticity_boost_;   // множитель обучения
    std::vector<int> low_rate_timer_;       // таймер для низкой активности
    
    struct PatternWill {
        std::vector<double> incoming;  // кто на меня влиял (пре)
        std::vector<double> outgoing;  // на кого я влиял (пост)
        double importance;             // накопленная важность
    };
    std::deque<PatternWill> will_pool_;  // "завещания" умерших нейронов
    static constexpr int MAX_WILL_POOL = 50;
    
    // ===== СВЯЗИ =====
    std::vector<std::vector<double>> W_;    // веса (size_ x size_)
    std::vector<Synapse> synapses_;         // синапсы для STDP
    PlasticityParams params_;               // параметры пластичности
    
    // ===== ВСПОМОГАТЕЛЬНЫЕ ПОЛЯ (для обратной совместимости) =====
    mutable std::vector<double> phi_cache_;
    mutable std::vector<double> pi_cache_;
    mutable bool cache_dirty_ = true;
    
    // ===== ELEVATION =====
    float elevation_ = 0.0f;
    float elevation_learning_rate_ = 0.001f;
    float elevation_decay_ = 0.999f;
    float cumulative_importance_ = 0.0f;
    int activity_counter_ = 0;
    
    // ===== ВНЕШНИЕ ЗАВИСИМОСТИ =====
    EmergentMemory* memory_manager_ = nullptr;
    bool is_input_group_ = false;
    bool is_self_model_group_ = false;
    std::shared_ptr<std::mt19937> rng_;  // shared_ptr
    // новые переменные для ии
    double conserved_energy_;  // "Lagrangian" группы
    std::vector<double> canonical_momentum_;  // обобщённый импульс
    
    // ===== ПРИВАТНЫЕ МЕТОДЫ =====
    void updateMembranePotentials();           // обновление V, спайки
    void updateTrophicSignals();               // расчёт трофинов
    void checkApoptosis();                     // проверка условий смерти
    void neurogenesis(int i);                  // рождение нового нейрона
    void inheritBestPattern(int i);            // наследование из will_pool_
    void buildSynapsesFromWeights();           // синхронизация W_ → synapses_
    void syncWeightsFromSynapses();            // синхронизация synapses_ → W_
    void updateCache() const;                  // обновление phi_cache_, pi_cache_
    int getSynapseIndex(int i, int j) const;   // индекс в линейном массиве
    
    // Вспомогательные
    double computeTrophicOutput(int i) const;  // сколько трофина выделяет нейрон
    double getFiringRateImpl(int i) const;     // частота спайков (Гц)

    // STDP LUT для оптимизации
    static const std::array<float, 21> getExpPlus() {
        static const std::array<float, 21> arr = []() {
            std::array<float, 21> a{};
            for (int dt = 0; dt <= 20; ++dt) {
                a[dt] = std::exp(-static_cast<float>(dt) / 20.0f);
            }
            return a;
        }();
        return arr;
    }
};

// ============================================================================
// ДЛЯ ОБРАТНОЙ СОВМЕСТИМОСТИ — заглушки старых структур
// ============================================================================

// Эти структуры больше не используются, но оставлены для компиляции
struct Vec3 { double x,y,z; Vec3() : x(0),y(0),z(0) {} };
struct OrbitalParams {};
struct MassLimits {};
struct HomeostasisParams {};

static double getOrbitRadiusStatic(int) { return 1.0; }
static std::vector<double> getAllOrbitRadii() { return {1.0}; }