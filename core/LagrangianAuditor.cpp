// core/LagrangianAuditor.cpp
#include "LagrangianAuditor.hpp"
#include "NeuralGroup.hpp"
#include "NeuralFieldSystem.hpp"
#include <cmath>
#include <iostream>

LagrangianAuditor::LagrangianAuditor() {
    reset();
}

LagrangianAuditor::LagrangianAuditor(const LagrangianAuditorConfig& cfg)
    : config_(cfg) {
    reset();
}

CanonicalState LagrangianAuditor::toCanonical(const std::vector<NeuralGroup>& groups,
                                               const std::vector<std::vector<double>>& interWeights,
                                               double dt) {
    const int N = (int)groups.size();
    CanonicalState state;
    state.resize(N);
    
    // Сохраняем предыдущие q для вычисления p
    static std::vector<double> prev_q(N, 0.5);
    static bool first_call = true;
    
    for (int i = 0; i < N; ++i) {
        // q = средняя активность группы (позиция)
        state.q[i] = groups[i].getAverageActivity();
        
        if (!first_call) {
            // p = производная q (импульс)
            state.p[i] = (state.q[i] - prev_q[i]) / dt;
            // Ограничиваем импульс
            state.p[i] = std::clamp(state.p[i], -10.0, 10.0);
        } else {
            state.p[i] = 0.0;
        }
        
        prev_q[i] = state.q[i];
    }
    
    first_call = false;
    
    // Вычисляем энергию
    state.total_energy = computeTotalEnergy(state, interWeights);
    
    return state;
}

double LagrangianAuditor::computeKineticEnergy(const CanonicalState& state) const {
    // T = ½ Σ p_i² / m (m=1 для простоты)
    double kinetic = 0.0;
    for (double p : state.p) {
        kinetic += 0.5 * p * p;
    }
    return kinetic;
}

double LagrangianAuditor::computePotentialEnergy(const CanonicalState& state,
                                                  const std::vector<std::vector<double>>& interWeights) const {
    // V = Σ w_ij * q_i * q_j (взаимодействие групп)
    double potential = 0.0;
    int N = (int)state.q.size();
    
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            if (i != j && i < (int)interWeights.size() && j < (int)interWeights[i].size()) {
                potential += interWeights[i][j] * state.q[i] * state.q[j];
            }
        }
    }
    
    // Добавляем "потенциальную яму" для удержания q в [0,1]
    for (double q : state.q) {
        potential += 10.0 * (q - 0.5) * (q - 0.5);  // параболическая яма
    }
    
    return potential;
}

double LagrangianAuditor::computeTotalEnergy(const CanonicalState& state,
                                              const std::vector<std::vector<double>>& interWeights) const {
    return computeKineticEnergy(state) + computePotentialEnergy(state, interWeights);
}

double LagrangianAuditor::computeLagrangian(const CanonicalState& state,
                                             const std::vector<std::vector<double>>& interWeights) const {
    // L = T - V
    return computeKineticEnergy(state) - computePotentialEnergy(state, interWeights);
}

double LagrangianAuditor::computeMomentumNorm(const CanonicalState& state) const {
    double norm = 0.0;
    for (double p : state.p) {
        norm += p * p;
    }
    return std::sqrt(norm);
}

void LagrangianAuditor::updateEma(double& ema, double new_value) {
    ema = ema * (1.0 - config_.energy_ema_alpha) + new_value * config_.energy_ema_alpha;
}

bool LagrangianAuditor::auditEnergyConservation(const CanonicalState& current_state,
                                                  const std::vector<std::vector<double>>& interWeights,
                                                  double dt) {
    // Сохраняем последнее состояние
    last_state_ = current_state;
    
    // Вычисляем компоненты энергии с переданными весами
    last_kinetic_ = computeKineticEnergy(current_state);
    last_potential_ = computePotentialEnergy(current_state, interWeights);
    last_total_energy_ = last_kinetic_ + last_potential_;
    
    double current_energy = current_state.total_energy;
    
    // Остальной код без изменений...
    energy_history_.push_back(current_energy);
    
    if (reference_energy_ == 0.0) {
        reference_energy_ = current_energy;
        return true;
    }
    
    double energy_change = std::abs(current_energy - reference_energy_);
    double relative_change = energy_change / (std::abs(reference_energy_) + 1e-9);
    
    updateEma(energy_error_ema_, relative_change);
    
    bool conserved = (relative_change < config_.energy_conservation_threshold);
    
    if (!conserved) {
        conservation_violations_++;
        
        if (conservation_violations_ % 10 == 1) {
            std::cout << "[LagrangianAuditor] ⚠️ Energy violation: ΔE=" 
                      << relative_change * 100 << "% (threshold " 
                      << config_.energy_conservation_threshold * 100 << "%)" << std::endl;
        }
    }
    
    reference_energy_ = reference_energy_ * 0.995 + current_energy * 0.005;
    
    return conserved;
}

float LagrangianAuditor::auditAction(const CanonicalState& before_state,
                                      const CanonicalState& after_state,
                                      const std::vector<std::vector<double>>& interWeights,
                                      double dt) {
    // Вычисляем энергии
    double E_before = computeTotalEnergy(before_state, interWeights);
    double E_after = computeTotalEnergy(after_state, interWeights);
    double delta_E = std::abs(E_after - E_before);
    double relative_delta = delta_E / (std::abs(E_before) + 1e-9);
    
    // Вычисляем изменение импульса
    double p_before = computeMomentumNorm(before_state);
    double p_after = computeMomentumNorm(after_state);
    double delta_p = std::abs(p_after - p_before);
    double relative_delta_p = delta_p / (p_before + 1e-9);
    
    // Вычисляем изменение Lagrangian
    double L_before = computeLagrangian(before_state, interWeights);
    double L_after = computeLagrangian(after_state, interWeights);
    double delta_L = std::abs(L_after - L_before);
    double relative_delta_L = delta_L / (std::abs(L_before) + 1e-9);
    
    // Риск галлюцинации = взвешенная сумма нарушений
    float risk = 0.0f;
    risk += static_cast<float>(std::min(1.0, relative_delta / config_.energy_conservation_threshold)) * 0.5f;
    risk += static_cast<float>(std::min(1.0, relative_delta_p / config_.momentum_conservation_threshold)) * 0.3f;
    risk += static_cast<float>(std::min(1.0, relative_delta_L / config_.action_threshold)) * 0.2f;
    
    risk = std::clamp(risk, 0.0f, 1.0f);
    
    // Логируем высокий риск
    if (risk > 0.7f) {
        std::cout << "[LagrangianAuditor] 🔴 HIGH RISK: ΔE=" << relative_delta * 100 
                  << "%, Δp=" << relative_delta_p * 100 
                  << "%, ΔL=" << relative_delta_L * 100 << "%" << std::endl;
    }
    
    return risk;
}

void LagrangianAuditor::correctState(CanonicalState& state,
                                      double target_energy,
                                      const std::vector<std::vector<double>>& interWeights) {
    double current_energy = computeTotalEnergy(state, interWeights);
    
    if (target_energy == 0.0) {
        target_energy = reference_energy_;
    }
    
    if (target_energy == 0.0) return;
    
    double delta_E = target_energy - current_energy;
    double correction = delta_E * config_.correction_strength;
    
    // Распределяем коррекцию на импульсы (меняем кинетическую энергию)
    double p_norm = computeMomentumNorm(state);
    if (p_norm > 1e-6) {
        double scale = std::sqrt((current_energy + correction) / (current_energy + 1e-9));
        scale = std::clamp(scale, 0.5, 1.5);
        for (double& p : state.p) {
            p *= scale;
        }
    } else {
        // Если импульс нулевой, добавляем небольшой толчок
        for (double& p : state.p) {
            p += correction * 0.1;
        }
    }
    
    // Пересчитываем энергию
    state.total_energy = computeTotalEnergy(state, interWeights);
    
    std::cout << "[LagrangianAuditor] 🔧 Corrected state: ΔE=" 
              << delta_E << " → new E=" << state.total_energy << std::endl;
}

std::vector<double> LagrangianAuditor::computePotentialGradient(
    const CanonicalState& state,
    const std::vector<std::vector<double>>& interWeights) const {
    
    int N = (int)state.q.size();
    std::vector<double> grad(N, 0.0);
    
    // ∂V/∂q_i = Σ_j (w_ij + w_ji) * q_j + 20 * (q_i - 0.5)
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            if (i != j && i < (int)interWeights.size() && j < (int)interWeights[i].size()) {
                grad[i] += (interWeights[i][j] + interWeights[j][i]) * state.q[j];
            }
        }
        // Потенциальная яма
        grad[i] += 20.0 * (state.q[i] - 0.5);
    }
    
    return grad;
}

CanonicalState LagrangianAuditor::hamiltonianStep(
    const CanonicalState& state,
    const std::vector<std::vector<double>>& interWeights,
    double dt) {
    
    CanonicalState next;
    next.resize(state.size());
    
    // Уравнения Гамильтона:
    // dq/dt = ∂H/∂p = p (так как H = ½p² + V(q))
    // dp/dt = -∂H/∂q = -∂V/∂q
    
    std::vector<double> potential_grad = computePotentialGradient(state, interWeights);
    
    for (size_t i = 0; i < state.q.size(); ++i) {
        // Полушаг для позиции (метод Верле для устойчивости)
        double p_half = state.p[i] - 0.5 * dt * potential_grad[i];
        next.q[i] = state.q[i] + dt * p_half;
        next.p[i] = p_half - 0.5 * dt * potential_grad[i];
        
        // Ограничения
        next.q[i] = std::clamp(next.q[i], 0.0, 1.0);
        next.p[i] = std::clamp(next.p[i], -10.0, 10.0);
    }
    
    next.total_energy = computeTotalEnergy(next, interWeights);
    
    return next;
}

void LagrangianAuditor::reset() {
    reference_energy_ = 0.0;
    energy_error_ema_ = 0.0;
    conservation_violations_ = 0;
    energy_history_.clear();
    momentum_history_.clear();
}