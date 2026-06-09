// core/LagrangianAuditor.hpp
#pragma once

#include <vector>
#include <deque>
#include <cmath>
#include <algorithm>
#include <numeric>

class NeuralFieldSystem;
class NeuralGroup;

/**
 * @struct CanonicalState
 * @brief Канонические координаты (q, p) для всей системы
 */
struct CanonicalState {
    std::vector<double> q;  // "позиции" — активности групп
    std::vector<double> p;  // "импульсы" — скорости изменения активностей
    double total_energy;     // полная энергия системы
    
    CanonicalState() : total_energy(0.0) {}
    
    void resize(size_t n) {
        q.resize(n, 0.0);
        p.resize(n, 0.0);
    }
    
    size_t size() const { return q.size(); }
};

/**
 * @struct LagrangianAuditorConfig
 * @brief Конфигурация аудитора на основе Lagrangian
 */
struct LagrangianAuditorConfig {
    // Пороги для обнаружения галлюцинаций
    double energy_conservation_threshold = 0.05;   // 5% изменение энергии
    double momentum_conservation_threshold = 0.10; // 10% изменение импульса
    double action_threshold = 0.15;                 // 15% изменение действия
    
    // Сглаживание (EMA)
    double energy_ema_alpha = 0.1;
    
    // Коррекция при нарушении
    bool auto_correct = true;
    double correction_strength = 0.1;
    
    // История для детекции трендов
    int history_size = 100;
};

/**
 * @class LagrangianAuditor
 * @brief Аудитор на основе законов сохранения (Lagrangian механика)
 * 
 * Принцип работы:
 * 1. Вычисляет полную энергию системы на каждом шаге
 * 2. Если энергия меняется > порога — обнаруживает галлюцинацию
 * 3. Может корректировать состояние для восстановления энергии
 */
class LagrangianAuditor {
public:
    LagrangianAuditor();
    explicit LagrangianAuditor(const LagrangianAuditorConfig& cfg);
    
    /**
     * @brief Преобразование сырых активностей в канонические координаты
     * @param groups Группы нейронов
     * @param interWeights Межгрупповые веса
     * @param dt Шаг времени
     * @return Каноническое состояние (q, p)
     */
    CanonicalState toCanonical(const std::vector<NeuralGroup>& groups,
                               const std::vector<std::vector<double>>& interWeights,
                               double dt);
    
    /**
     * @brief Вычисление Lagrangian для текущего состояния
     * @param state Каноническое состояние
     * @param interWeights Межгрупповые веса
     * @return Значение Lagrangian L = T - V
     */
    double computeLagrangian(const CanonicalState& state,
                             const std::vector<std::vector<double>>& interWeights) const;
    
    /**
     * @brief Вычисление полной энергии (Hamiltonian)
     * @param state Каноническое состояние
     * @param interWeights Межгрупповые веса
     * @return Полная энергия H = T + V
     */
    double computeTotalEnergy(const CanonicalState& state,
                              const std::vector<std::vector<double>>& interWeights) const;
    
    /**
     * @brief Проверка сохранения энергии (основной метод аудита)
     * @param current_state Текущее состояние
     * @param dt Шаг времени
     * @return true если энергия сохраняется, false если галлюцинация
     */
    bool auditEnergyConservation(const CanonicalState& current_state, 
                             const std::vector<std::vector<double>>& interWeights, 
                             double dt);
    
    /**
     * @brief Полный аудит действия агента
     * @param before_state Состояние до действия
     * @param after_state Состояние после действия
     * @param interWeights Межгрупповые веса
     * @param dt Шаг времени
     * @return Риск галлюцинации (0-1)
     */
    float auditAction(const CanonicalState& before_state,
                      const CanonicalState& after_state,
                      const std::vector<std::vector<double>>& interWeights,
                      double dt);
    
    /**
     * @brief Коррекция состояния для сохранения энергии
     * @param state Состояние для коррекции (изменяется на месте)
     * @param target_energy Целевая энергия (если 0, использует предыдущую)
     * @param interWeights Межгрупповые веса
     */
    void correctState(CanonicalState& state,
                      double target_energy,
                      const std::vector<std::vector<double>>& interWeights);
    
    /**
     * @brief Предсказание следующего состояния через уравнения Гамильтона
     * @param state Текущее состояние
     * @param interWeights Межгрупповые веса
     * @param dt Шаг времени
     * @return Предсказанное состояние
     */
    CanonicalState hamiltonianStep(const CanonicalState& state,
                                   const std::vector<std::vector<double>>& interWeights,
                                   double dt);
    
    // Геттеры
    double getReferenceEnergy() const { return reference_energy_; }
    double getEnergyError() const { return energy_error_ema_; }
    double getConservationViolations() const { return conservation_violations_; }
    const std::deque<double>& getEnergyHistory() const { return energy_history_; }

    // НОВЫЕ ГЕТТЕРЫ:
    double getLastKinetic() const { return last_kinetic_; }
    double getLastPotential() const { return last_potential_; }
    double getLastTotalEnergy() const { return last_total_energy_; }
    const CanonicalState& getLastState() const { return last_state_; }
    
    // Сброс
    void reset();
    
    // Настройка
    void setConfig(const LagrangianAuditorConfig& cfg) { config_ = cfg; }
    const LagrangianAuditorConfig& getConfig() const { return config_; }
    LagrangianAuditorConfig& getConfigNonConst() { return config_; }
    
private:
    LagrangianAuditorConfig config_;
    
    double reference_energy_ = 0.0;
    double energy_error_ema_ = 0.0;
    int conservation_violations_ = 0;
    
    std::deque<double> energy_history_;
    std::deque<double> momentum_history_;

    // НОВЫЕ ПОЛЯ ДЛЯ ХРАНЕНИЯ ПОСЛЕДНИХ ЗНАЧЕНИЙ:
    double last_kinetic_ = 0.0;
    double last_potential_ = 0.0;
    double last_total_energy_ = 0.0;
    CanonicalState last_state_;
    
    // Вспомогательные методы
    double computeKineticEnergy(const CanonicalState& state) const;
    double computePotentialEnergy(const CanonicalState& state,
                                  const std::vector<std::vector<double>>& interWeights) const;
    double computeMomentumNorm(const CanonicalState& state) const;
    void updateEma(double& ema, double new_value);
    
    // Градиенты для уравнений Гамильтона
    std::vector<double> computePotentialGradient(const CanonicalState& state,
                                                  const std::vector<std::vector<double>>& interWeights) const;
};