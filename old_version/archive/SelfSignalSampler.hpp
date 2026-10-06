#pragma once
// SelfSignalSampler.hpp
//
// Адаптирован для новой архитектуры (без орбит, с LIF нейронами)
//
// Собирает статистику о состоянии системы и инжектирует её обратно
// в self-model группы (28-31) для создания замкнутого контура самонаблюдения.
//
// Сигналы (32 штуки):
//   - Активность нейронов (средняя частота спайков по группам) — 16 сигналов
//   - Память (STM/LTM заполненность, важность) — 4 сигнала
//   - Обучение (surprise, quality, exploration) — 4 сигнала
//   - Гомеостаз (энтропия, температура, трофины) — 4 сигнала
//   - Диагностика (апоптоз, консолидация) — 4 сигнала

#include <vector>
#include <deque>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <array>

// Forward declarations
class NeuralFieldSystem;
struct EmergentSignal;

// ============================================================================
// СНАПШОТ СОСТОЯНИЯ СИСТЕМЫ (32 сигнала)
// ============================================================================

struct SelfSnapshot {
    // ── Активность нейронов (16 сигналов) ────────────────────────────────
    // Средняя частота спайков для каждой из 16 ключевых групп
    // (сенсорные 1-3, моторные 4-7, семантические 16-21, остальные объединены)
    float sensory_avg_rate;      // средняя частота сенсорных групп (1-3)
    float motor_avg_rate;        // средняя частота моторных групп (4-7)
    std::array<float, 6> semantic_rates;  // частоты семантических групп (16-21)
    std::array<float, 4> associative_rates; // частоты ассоциативных групп (8-11, 12-15 объединены)
    float context_avg_rate;      // средняя частота контекстных групп (22-27)
    float self_model_avg_rate;   // средняя частота self-model групп (28-31)
    
    // ── Память (4 сигнала) ────────────────────────────────────────────────
    float stm_fullness;          // заполненность STM (0-1)
    float ltm_fullness;          // заполненность LTM (0-1)
    float ltm_avg_importance;    // средняя важность записей в LTM
    float memory_consolidation_rate; // скорость консолидации (EMA)
    
    // ── Обучение (4 сигнала) ──────────────────────────────────────────────
    float surprise;              // неожиданность (из EmergentSignal)
    float quality;               // качество текущего состояния
    float improvement_trend;     // тренд улучшения
    float exploration_pressure;  // давление к исследованию (surprise + boredom)
    
    // ── Гомеостаз (4 сигнала) ────────────────────────────────────────────
    float system_entropy;        // энтропия системы [0,1]
    float entropy_error;         // отклонение от целевой энтропии
    float attention_temperature; // температура внимания (нормализованная)
    float avg_trophic_level;     // средний уровень трофинов
    
    // ── Диагностика (4 сигнала) ──────────────────────────────────────────
    float apoptosis_rate;        // скорость апоптоза (смертей нейронов)
    float neurogenesis_rate;     // скорость нейрогенеза (рождений)
    float avg_firing_rate;       // средняя частота спайков по всем нейронам
    float network_activity;      // общая активность сети (доля активных нейронов)

    // НОВЫЕ СИГНАЛЫ ДЛЯ ЭНЕРГИИ (добавить в конец)
    float system_energy;           // полная энергия системы
    float energy_conservation_error; // ошибка сохранения энергии
    float hamiltonian_risk;        // риск на основе нарушения Гамильтона
    
    // ── Преобразование в плоский вектор (32 float) ───────────────────────
    std::vector<float> toVector() const {
        std::vector<float> result;
        result.reserve(32);
        
        // Активность (16)
        result.push_back(sensory_avg_rate);
        result.push_back(motor_avg_rate);
        for (float v : semantic_rates) result.push_back(v);
        for (float v : associative_rates) result.push_back(v);
        result.push_back(context_avg_rate);
        result.push_back(self_model_avg_rate);
        
        // Память (4)
        result.push_back(stm_fullness);
        result.push_back(ltm_fullness);
        result.push_back(ltm_avg_importance);
        result.push_back(memory_consolidation_rate);
        
        // Обучение (4)
        result.push_back(surprise);
        result.push_back(quality);
        result.push_back(improvement_trend);
        result.push_back(exploration_pressure);
        
        // Гомеостаз (4)
        result.push_back(system_entropy);
        result.push_back(entropy_error);
        result.push_back(attention_temperature);
        result.push_back(avg_trophic_level);
        
        // Диагностика (4)
        result.push_back(apoptosis_rate);
        result.push_back(neurogenesis_rate);
        result.push_back(avg_firing_rate);
        result.push_back(network_activity);
        
        return result;
    }
    
    // Конструктор с значениями по умолчанию
    SelfSnapshot() {
        sensory_avg_rate = 0.0f;
        motor_avg_rate = 0.0f;
        semantic_rates.fill(0.0f);
        associative_rates.fill(0.0f);
        context_avg_rate = 0.0f;
        self_model_avg_rate = 0.0f;
        
        stm_fullness = 0.0f;
        ltm_fullness = 0.0f;
        ltm_avg_importance = 0.0f;
        memory_consolidation_rate = 0.0f;
        
        surprise = 0.0f;
        quality = 0.0f;
        improvement_trend = 0.0f;
        exploration_pressure = 0.0f;
        
        system_entropy = 0.0f;
        entropy_error = 0.0f;
        attention_temperature = 0.0f;
        avg_trophic_level = 0.0f;
        
        apoptosis_rate = 0.0f;
        neurogenesis_rate = 0.0f;
        avg_firing_rate = 0.0f;
        network_activity = 0.0f;
        // Новые поля
        system_energy = 0.0f;
        energy_conservation_error = 0.0f;
        hamiltonian_risk = 0.0f;
    }
};

// ============================================================================
// SELF SIGNAL SAMPLER — НОВАЯ ВЕРСИЯ
// ============================================================================

class SelfSignalSampler {
public:
    // Группы для инжекции self-signals
    static constexpr int SELF_GROUP_START = 28;
    static constexpr int SELF_GROUP_COUNT = 4;      // группы 28, 29, 30, 31
    static constexpr int SIGNALS_PER_GROUP = 8;     // 4 × 8 = 32 сигнала
    static constexpr int TOTAL_SIGNALS = SELF_GROUP_COUNT * SIGNALS_PER_GROUP; // = 32
    
    // Сила инжекции (0 = не влияет, 1 = полностью заменяет)
    float injection_strength = 0.25f;
    
    // Константы для EMA
    float ema_alpha = 0.05f;
    
    SelfSignalSampler();
    
    // Сбор снапшота (вызывается после эволюции нейронов)
    SelfSnapshot sample(const NeuralFieldSystem& sys,
                        const EmergentSignal&    sig,
                        int                      step);
    
    // Инжекция снапшота в self-model группы
    void inject(NeuralFieldSystem& sys, const SelfSnapshot& snap);
    
    // Получить последний снапшот (для диагностики)
    const SelfSnapshot& last() const { return last_snap_; }
    
    // Сброс истории (при перезагрузке системы)
    void reset();
    
private:
    SelfSnapshot last_snap_{};
    
    // EMA для сглаживания
    float apoptosis_ema_ = 0.0f;
    float neurogenesis_ema_ = 0.0f;
    float consolidation_ema_ = 0.0f;
    mutable float firing_rate_ema_ = 0.0f;
    
    // История для вычисления трендов
    std::deque<float> quality_history_;
    std::deque<float> entropy_history_;
    std::deque<int> apoptosis_history_;
    std::deque<int> neurogenesis_history_;
    
    // Последние значения для вычисления дельт
    int last_apoptosis_count_ = 0;
    int last_neurogenesis_count_ = 0;
    size_t last_ltm_size_ = 0;
    float last_avg_firing_rate_ = 0.0f;
    int step_counter_ = 0;
    
    // Вспомогательные методы
    float computeEma(float old_val, float new_val) const;
    float computeTrend(const std::deque<float>& history, int window) const;
    float computeActivityLevel(const NeuralFieldSystem& sys) const;
    float computeAverageFiringRate(const NeuralFieldSystem& sys) const;
    float computeAverageTrophicLevel(const NeuralFieldSystem& sys) const;
    int countApoptosisEvents(const NeuralFieldSystem& sys) const;
    int countNeurogenesisEvents(const NeuralFieldSystem& sys) const;
    
    // Константы
    static constexpr int TREND_WINDOW = 50;
    static constexpr int HISTORY_MAX = 200;
};