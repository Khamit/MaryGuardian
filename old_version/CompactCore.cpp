// core/CompactCore.cpp
// Реализация компактного динамического ядра

#include "CompactCore.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace orchestrator {

// ============================================================================
// ПУБЛИЧНЫЕ МЕТОДЫ
// ============================================================================

std::vector<float> CompactCore::step(const std::vector<float>& input) {
    // Сохраняем вход для обучения
    last_input_ = input;
    last_input_.resize(cfg_.input_dim, 0.0f);

    // 1. Предварительная активация (pre-activation)
    for (int i = 0; i < cfg_.hidden_dim; ++i) {
        float v = b_hidden_[i];

        // Входной слой
        for (int j = 0; j < cfg_.input_dim; ++j) {
            v += W_in_[i][j] * last_input_[j];
        }

        // Рекуррентный слой
        for (int j = 0; j < cfg_.hidden_dim; ++j) {
            v += W_rec_[i][j] * state_[j];
        }

        last_pre_[i] = v;
    }

    // 2. Активация + leaky интегратор
    //    state ← (1 - decay) * state + decay * tanh(pre) — low-pass filter
    for (int i = 0; i < cfg_.hidden_dim; ++i) {
        const float activated = std::tanh(last_pre_[i]);
        state_[i] = (1.0f - cfg_.decay) * state_[i] + cfg_.decay * activated;

        // Дополнительная стабилизация: мягкое ограничение, если state_
        // выходит за разумные пределы (защита от накопления ошибок округления)
        if (std::abs(state_[i]) > 2.0f) {
            state_[i] = std::tanh(state_[i]) * 1.5f;
        }
    }

    // 3. Выходной слой
    for (int i = 0; i < cfg_.output_dim; ++i) {
        float v = b_out_[i];
        for (int j = 0; j < cfg_.hidden_dim; ++j) {
            v += W_out_[i][j] * state_[j];
        }
        last_output_[i] = std::tanh(v);
    }

    return last_output_;
}

void CompactCore::learn(float reward, const std::vector<float>& target_output) {
    // Ограничиваем reward, чтобы не было слишком сильных скачков
    reward = std::clamp(reward, -1.0f, 1.0f);

    // ========================================================================
    // 1. ВЫХОДНОЙ СЛОЙ (output layer)
    // ========================================================================
    for (int i = 0; i < cfg_.output_dim; ++i) {
        float error;
        if (!target_output.empty() && i < static_cast<int>(target_output.size())) {
            // Супервизорное обучение: ошибка = target - output
            error = target_output[i] - last_output_[i];
        } else {
            // Reward-modulated Hebbian
            error = reward * last_output_[i];
        }

        // Производная tanh: 1 - tanh²
        const float d_out = error * (1.0f - last_output_[i] * last_output_[i]);

        for (int j = 0; j < cfg_.hidden_dim; ++j) {
            float w = W_out_[i][j];
            w = w * (1.0f - cfg_.weight_decay);
            w += cfg_.lr * d_out * state_[j];
            W_out_[i][j] = clip(w);
        }

        float b = b_out_[i];
        b = b * (1.0f - cfg_.weight_decay);
        b += cfg_.lr * d_out;
        b_out_[i] = clip(b);
    }

    // ========================================================================
    // 2. СКРЫТЫЙ СЛОЙ (hidden layer) — входные веса
    // ========================================================================
    std::vector<float> hidden_delta(cfg_.hidden_dim, 0.0f);

    for (int i = 0; i < cfg_.hidden_dim; ++i) {
        const float t = std::tanh(last_pre_[i]);
        const float tanh_prime = 1.0f - t * t;

        // Backprop от выходного слоя
        float backprop_sum = 0.0f;
        for (int k = 0; k < cfg_.output_dim; ++k) {
            float output_error = (!target_output.empty() && k < static_cast<int>(target_output.size()))
                ? (target_output[k] - last_output_[k])
                : (reward * last_output_[k]);

            float d_out = output_error * (1.0f - last_output_[k] * last_output_[k]);
            backprop_sum += d_out * W_out_[k][i];
        }

        // Локальная компонента (Hebbian)
        float local_component = reward * t;

        // 70% backprop + 30% локальное Hebbian
        hidden_delta[i] = tanh_prime * (0.7f * backprop_sum + 0.3f * local_component);
        hidden_delta[i] = std::clamp(hidden_delta[i], -1.0f, 1.0f);
    }

    // Обновляем входные веса
    for (int i = 0; i < cfg_.hidden_dim; ++i) {
        for (int j = 0; j < cfg_.input_dim; ++j) {
            float w = W_in_[i][j];
            w = w * (1.0f - cfg_.weight_decay);
            w += cfg_.lr * hidden_delta[i] * last_input_[j];
            W_in_[i][j] = clip(w);
        }

        float b = b_hidden_[i];
        b = b * (1.0f - cfg_.weight_decay);
        b += 0.5f * cfg_.lr * hidden_delta[i];
        b_hidden_[i] = clip(b);
    }

    // ========================================================================
    // 3. РЕКУРРЕНТНЫЕ ВЕСА (recurrent weights)
    // ========================================================================
    const float rec_decay = cfg_.weight_decay * 3.0f;
    const float rec_lr = cfg_.lr * 0.1f;

    for (int i = 0; i < cfg_.hidden_dim; ++i) {
        for (int j = 0; j < cfg_.hidden_dim; ++j) {
            if (i == j) {
                // Самосвязи сильно ограничиваем — легко ведут к насыщению
                float w = W_rec_[i][j];
                w = w * (1.0f - rec_decay * 2.0f);
                w += rec_lr * hidden_delta[i] * state_[j] * 0.1f;
                W_rec_[i][j] = clip(w * 0.5f);
            } else {
                float w = W_rec_[i][j];
                w = w * (1.0f - rec_decay);
                w += rec_lr * hidden_delta[i] * state_[j];
                W_rec_[i][j] = clip(w);
            }
        }
    }

    // ========================================================================
    // 4. ПОСТ-ОБРАБОТКА: спектральная нормализация
    // ========================================================================
    static int step_count = 0;
    step_count++;

    if (step_count % 100 == 0) {
        spectralNormalizeRecurrent();
    }
}

// ============================================================================
// ДИАГНОСТИКА И ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ
// ============================================================================

float CompactCore::getActivityNorm() const {
    if (state_.empty()) return 0.0f;

    float sum_sq = 0.0f;
    for (float v : state_) {
        sum_sq += v * v;
    }
    return std::sqrt(sum_sq / static_cast<float>(state_.size()));
}

float CompactCore::getSpectralRadius() const {
    const int n = cfg_.hidden_dim;
    if (n == 0) return 0.0f;

    std::vector<float> v(n, 1.0f / std::sqrt(static_cast<float>(n)));

    float norm = 0.0f;
    for (float val : v) norm += val * val;
    norm = std::sqrt(norm);
    for (float& val : v) val /= norm;

    for (int iter = 0; iter < 10; ++iter) {
        std::vector<float> v_new(n, 0.0f);
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                v_new[i] += W_rec_[i][j] * v[j];
            }
        }

        float new_norm = 0.0f;
        for (float val : v_new) new_norm += val * val;
        new_norm = std::sqrt(new_norm);

        if (new_norm < 1e-6f) return 0.0f;

        for (float& val : v_new) val /= new_norm;
        v = std::move(v_new);
    }

    float radius = 0.0f;
    for (int i = 0; i < n; ++i) {
        float sum = 0.0f;
        for (int j = 0; j < n; ++j) {
            sum += W_rec_[i][j] * v[j];
        }
        radius = std::max(radius, std::abs(sum));
    }

    return radius;
}

void CompactCore::reset() {
    std::fill(state_.begin(), state_.end(), 0.0f);
    std::fill(last_input_.begin(), last_input_.end(), 0.0f);
    std::fill(last_pre_.begin(), last_pre_.end(), 0.0f);
    std::fill(last_output_.begin(), last_output_.end(), 0.0f);
}

// ============================================================================
// ПРИВАТНЫЕ МЕТОДЫ
// ============================================================================

float CompactCore::clip(float v) const {
    return std::clamp(v, -cfg_.weight_clip, cfg_.weight_clip);
}

void CompactCore::spectralNormalizeRecurrent() {
    float radius = getSpectralRadius();

    if (radius > 0.95f) {
        float scale = 0.95f / radius;
        for (auto& row : W_rec_) {
            for (float& w : row) {
                w *= scale;
            }
        }
    }
}

} // namespace orchestrator