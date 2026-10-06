#pragma once
// core/CompactCore.hpp
//
// Компактное динамическое ядро — замена 1024-нейронной LIF/STDP/апоптоз/
// нейрогенез/Lagrangian симуляции.
//
// ВАЖНО: это декларативный заголовок. Реализация step()/learn() и
// диагностических методов — в CompactCore.cpp. Раньше эти методы были
// определены ВТОРОЙ РАЗ прямо в теле класса в этом файле, что давало
// redefinition error и не давало доступа к getSpectralRadius()/
// spectralNormalizeRecurrent(), которые main.cpp уже вызывает.
//
// Роль ядра (раздел 7 рекомендации по графовой памяти):
//   1. Compression      — graph_embedding + agent_signal -> latent state
//   2. Sensory reaction  — output[*] реагирует на аномалии во входе
//   3. Local control     — output интерпретируется как boost/suppress/reroute

#include <vector>
#include <cmath>
#include <random>
#include <algorithm>

namespace orchestrator {

class CompactCore {
public:
    struct Config {
        int input_dim    = 52;  // getGlobalEmbedding(32) + getInteractionEmbedding(16) + getAgentEmbedding(4)
        int hidden_dim   = 64;  // размер скрытого состояния
        int output_dim   = 12;  // управляющие сигналы

        float decay       = 0.15f; // скорость "забывания" скрытого состояния
        float lr          = 0.01f; // скорость онлайн-обучения
        float weight_clip = 1.0f;  // ограничение весов

        // Синаптическая нормализация (weight decay) — простая, принципиальная
        // альтернатива Lagrangian energy conservation: без неё Hebbian-правило
        // в learn() монотонно увеличивает |W|, скрытое состояние уходит в
        // tanh-насыщение, производная tanh' -> 0, обучение останавливается.
        float weight_decay = 0.01f;
    };

    explicit CompactCore(Config cfg, unsigned seed = 1234)
        : cfg_(cfg), rng_(seed) {
        state_.assign(cfg_.hidden_dim, 0.0f);

        W_in_.assign(cfg_.hidden_dim, std::vector<float>(cfg_.input_dim, 0.0f));
        W_rec_.assign(cfg_.hidden_dim, std::vector<float>(cfg_.hidden_dim, 0.0f));
        W_out_.assign(cfg_.output_dim, std::vector<float>(cfg_.hidden_dim, 0.0f));
        b_hidden_.assign(cfg_.hidden_dim, 0.0f);
        b_out_.assign(cfg_.output_dim, 0.0f);

        // Xavier-подобная инициализация: масштаб ~1/sqrt(размер входа),
        // чтобы сумма входов в нейрон не выводила tanh в насыщение со старта.
        const float in_scale  = 1.0f / std::sqrt(static_cast<float>(cfg_.input_dim));
        const float rec_scale = 1.0f / std::sqrt(static_cast<float>(cfg_.hidden_dim));

        std::uniform_real_distribution<float> dist_in(-in_scale, in_scale);
        std::uniform_real_distribution<float> dist_rec(-rec_scale, rec_scale);

        for (auto& row : W_in_)  for (auto& w : row) w = dist_in(rng_);
        // Рекуррентные веса инициализируем заметно мельче входных — иначе
        // коррелированное Hebbian-обновление по всем юнитам толкает state_
        // в самоподдерживающееся насыщение (спектральный радиус W_rec
        // должен быть < 1; см. spectralNormalizeRecurrent()).
        for (auto& row : W_rec_) for (auto& w : row) w = dist_rec(rng_) * 0.1f;
        for (auto& row : W_out_) for (auto& w : row) w = dist_rec(rng_);

        last_input_.assign(cfg_.input_dim, 0.0f);
        last_pre_.assign(cfg_.hidden_dim, 0.0f);
        last_output_.assign(cfg_.output_dim, 0.0f);
    }

    // Один шаг динамики. input.size() == cfg_.input_dim (короче — паддится нулями).
    std::vector<float> step(const std::vector<float>& input);

    // Онлайн-обучение от реального reward (исход задачи из AgentGraph).
    void learn(float reward, const std::vector<float>& target_output = {});

    // ----- Доступ к состоянию ---------------------------------------------
    const std::vector<float>& getState() const { return state_; }
    const std::vector<float>& getLastOutput() const { return last_output_; }

    // ----- Диагностика стабильности ----------------------------------------
    // Норма скрытого состояния — простая замена Lagrangian energy conservation.
    float getActivityNorm() const;

    // Приблизительный спектральный радиус W_rec (power iteration).
    // > 1 означает потенциально неустойчивую рекуррентную динамику.
    float getSpectralRadius() const;

    void reset();

    const Config& config() const { return cfg_; }

private:
    float clip(float v) const;

    // Нормализует W_rec так, чтобы спектральный радиус <= 0.95.
    // Вызывается периодически из learn().
    void spectralNormalizeRecurrent();

    Config cfg_;
    std::mt19937 rng_;

    std::vector<float> state_;
    std::vector<std::vector<float>> W_in_, W_rec_, W_out_;
    std::vector<float> b_hidden_, b_out_;

    std::vector<float> last_input_, last_pre_, last_output_;
};

} // namespace orchestrator