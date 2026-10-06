#pragma once
#include <vector>
#include <deque>
#include <random>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <iostream>
#include <tuple>

namespace nc {

struct Config {
    int N = 256;
    int n_in = 32;
    int n_motor = 32;
    float dt = 1.0f;

    float V_rest = -70.0f;
    float V_reset = -80.0f;
    float V_thresh0 = -52.0f;
    float V_thresh_max = -35.0f;
    float C = 1.0f;
    float g_leak = 0.1f;
    float I_noise = 0.25f;

    float I_bias_input  = 0.30f;
    float I_bias_hidden = 1.10f;
    float I_bias_motor  = 1.00f;

    int refractory = 1;
    float adapt = 1.5f;
    float thresh_decay = 0.995f;

    float tau_pre = 15.0f;
    float tau_post = 15.0f;
    float A_plus = 0.01f;         // СЛАБЕЕ STDP
    float A_minus = 0.0105f;
    float el_decay = 0.95f;       // МЕДЛЕННЕЕ затухание eligibility
    float lr = 0.01f;             // МЕНЬШЕ learning rate
    float w_max = 2.0f;
    float w_min = -2.0f;
    float w0 = 0.15f;

    float trophic_decay = 0.998f;
    float trophic_spike = 0.05f;
    float trophic_death = 0.15f;
    int death_delay = 2000;
    int crit_period = 100;
    float boost = 3.0f;

    float inherit = 0.70f;
    float mutate = 0.08f;
    int will_size = 50;

    // Exploration drive для моторного слоя
    float exploration_boost = 2.0f;
    int exploration_episodes = 2000;
};

struct Neuron {
    float V = -70.0f;
    float V_thresh = -52.0f;
    bool spike = false;
    int refractory = 0;
    float pre = 0.0f;
    float post = 0.0f;
    float trophic = 1.0f;
    int last_spike = -1000;
    int age = 0;
    int death_timer = 0;
};

struct Genome {
    std::vector<float> w_in;
    std::vector<float> w_out;
    float fitness = 0.0f;
};

class Core {
public:
    explicit Core(const Config& c = Config{});

    void set_input(const std::vector<float>& currents);
    void clear_input();
    void step(int step_num);
    void modulate(float modulation);
    void set_episode(int ep);

    int motor_spike_count(int window) const;
    float mean_rate_window(int window) const;
    void stats(int step, int window = 20) const;
    std::tuple<int,int,int> get_layer_spikes() const;

private:
    Config cfg;
    int N, n_hidden;
    int in0, in1, hid0, hid1, mot0, mot1;
    std::vector<Neuron> n;
    std::vector<float> w;
    std::vector<float> e;
    std::vector<float> I_ext;
    std::deque<Genome> pool;
    std::mt19937 rng;
    int step_counter = 0;
    int current_episode = 0;

    void update_lif();
    void update_stdp();
    void update_trophic();
    void update_apoptosis();
    void neurogenesis(int idx, const Genome& g);
    Genome compress(int idx);
    float get_bias(int idx) const;
};

// ============================================================================
// РЕАЛИЗАЦИЯ
// ============================================================================

inline Core::Core(const Config& c) : cfg(c), N(c.N), rng(42) {
    n.resize(N);
    for (int i = 0; i < N; ++i) {
        n[i].V = cfg.V_rest;
        n[i].V_thresh = cfg.V_thresh0;
    }

    w.assign(N * N, 0.0f);
    e.assign(N * N, 0.0f);
    I_ext.assign(cfg.n_in, 0.0f);

    n_hidden = N - cfg.n_in - cfg.n_motor;
    in0 = 0;               in1 = cfg.n_in;
    hid0 = cfg.n_in;       hid1 = cfg.n_in + n_hidden;
    mot0 = N - cfg.n_motor; mot1 = N;

    std::uniform_real_distribution<float> d_exc(0.10f, 0.30f);
    std::uniform_real_distribution<float> d_weak(-0.08f, 0.08f);
    std::uniform_real_distribution<float> d_inh(-0.15f, -0.05f);

    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            if (i == j) continue;
            bool i_in  = (i >= in0  && i < in1);
            bool i_hid = (i >= hid0 && i < hid1);
            bool i_mot = (i >= mot0 && i < mot1);
            bool j_in  = (j >= in0  && j < in1);
            bool j_hid = (j >= hid0 && j < hid1);
            bool j_mot = (j >= mot0 && j < mot1);

            if (i_in && j_hid) {
                w[i*N+j] = d_exc(rng);
            } else if (i_hid && j_mot) {
                w[i*N+j] = d_exc(rng);
            } else if (i_hid && j_hid) {
                w[i*N+j] = d_weak(rng);
            } else if (i_mot && j_hid) {
                w[i*N+j] = d_weak(rng) * 0.3f;
            } else if (i_in && j_in) {
                w[i*N+j] = 0.0f;
            } else if (i_mot && j_mot) {
                w[i*N+j] = d_weak(rng) * 0.5f;
            } else {
                w[i*N+j] = 0.0f;
            }
        }
    }
}

inline void Core::set_episode(int ep) {
    current_episode = ep;
}

inline float Core::get_bias(int idx) const {
    float bias = 0.0f;
    if (idx < in1) bias = cfg.I_bias_input;
    else if (idx < hid1) bias = cfg.I_bias_hidden;
    else bias = cfg.I_bias_motor;

    // Exploration drive: временно повышаем ток моторных нейронов
    if (idx >= mot0 && idx < mot1 && current_episode < cfg.exploration_episodes) {
        bias += cfg.exploration_boost;
    }
    return bias;
}

inline void Core::set_input(const std::vector<float>& currents) {
    size_t lim = std::min(currents.size(), (size_t)cfg.n_in);
    for (size_t i = 0; i < lim; ++i) I_ext[i] = currents[i];
    for (size_t i = lim; i < I_ext.size(); ++i) I_ext[i] = 0.0f;
}

inline void Core::clear_input() {
    std::fill(I_ext.begin(), I_ext.end(), 0.0f);
}

inline void Core::step(int step_num) {
    step_counter = step_num;

    float d_pre = std::exp(-cfg.dt / cfg.tau_pre);
    float d_post = std::exp(-cfg.dt / cfg.tau_post);
    for (int i = 0; i < N; ++i) {
        n[i].pre *= d_pre;
        n[i].post *= d_post;
    }

    update_lif();
    update_stdp();

    for (int i = 0; i < N; ++i) {
        if (n[i].spike) {
            n[i].pre += 1.0f;
            n[i].post += 1.0f;
        }
    }

    for (float& val : e) val *= cfg.el_decay;
    update_trophic();
    // Апоптоз отключён для первого теста
    // update_apoptosis();
}

inline void Core::update_lif() {
    std::normal_distribution<float> noise(0.0f, cfg.I_noise);
    std::vector<bool> spike_next(N, false);

    // Уменьшаем рефрактерность для всех
    for (int i = 0; i < N; ++i) {
        if (n[i].refractory > 0) {
            n[i].refractory--;
        }
    }

    // Вычисляем следующие состояния синхронно
    for (int i = 0; i < N; ++i) {
        auto& nr = n[i];
        if (nr.refractory > 0) {
            nr.V = cfg.V_reset;
            spike_next[i] = false;
            continue;
        }

        float I_syn = 0.0f;
        for (int j = 0; j < N; ++j) {
            if (j == i) continue;
            if (n[j].spike) {  // spikes от предыдущего шага
                I_syn += w[j * N + i];
            }
        }

        float I_in = 0.0f;
        if (i >= in0 && i < in1) {
            I_in = I_ext[i - in0] * 4.0f;
        }

        float I_leak = -cfg.g_leak * (nr.V - cfg.V_rest);
        float I_bias_local = get_bias(i);
        float dV = cfg.dt * (I_syn + I_leak + I_bias_local + I_in + noise(rng)) / cfg.C;
        nr.V += dV;
        nr.age++;

        if (nr.V > nr.V_thresh) {
            spike_next[i] = true;
            nr.V = cfg.V_reset;
            nr.refractory = cfg.refractory;
            nr.last_spike = step_counter;
            nr.V_thresh += cfg.adapt;
            nr.V_thresh = std::min(nr.V_thresh, cfg.V_thresh_max);
        } else {
            spike_next[i] = false;
            nr.V_thresh = nr.V_thresh * cfg.thresh_decay + cfg.V_thresh0 * (1.0f - cfg.thresh_decay);
        }
    }

    // Применяем spikes синхронно
    for (int i = 0; i < N; ++i) {
        n[i].spike = spike_next[i];
    }
}

inline void Core::update_stdp() {
    for (int j = 0; j < N; ++j) {
        if (!n[j].spike) continue;
        for (int i = 0; i < N; ++i) {
            if (i == j) continue;
            int idx = i * N + j;
            e[idx] += cfg.A_plus * n[i].pre;
            e[idx] = std::clamp(e[idx], -1.0f, 1.0f);
        }
    }
    for (int i = 0; i < N; ++i) {
        if (!n[i].spike) continue;
        for (int j = 0; j < N; ++j) {
            if (i == j) continue;
            int idx = i * N + j;
            e[idx] -= cfg.A_minus * n[j].post;
            e[idx] = std::clamp(e[idx], -1.0f, 1.0f);
        }
    }
}

inline void Core::update_trophic() {
    for (int i = 0; i < N; ++i) {
        if (n[i].spike) n[i].trophic += cfg.trophic_spike;
        n[i].trophic *= cfg.trophic_decay;

        float support = 0.0f;
        for (int j = 0; j < N; ++j) {
            if (j == i) continue;
            if (n[j].spike) support += 0.008f * std::abs(w[j * N + i]);
        }
        n[i].trophic += support;
        if (n[i].trophic > 10.0f) n[i].trophic = 10.0f;
    }
}

inline void Core::update_apoptosis() {
    for (int i = 0; i < N; ++i) {
        if (n[i].trophic < cfg.trophic_death) {
            n[i].death_timer++;
        } else {
            n[i].death_timer = 0;
        }

        if (n[i].death_timer > cfg.death_delay) {
            Genome g = compress(i);
            pool.push_back(g);
            if ((int)pool.size() > cfg.will_size) pool.pop_front();

            if (!pool.empty()) {
                const Genome* best = &pool[0];
                for (const auto& cand : pool) if (cand.fitness > best->fitness) best = &cand;
                neurogenesis(i, *best);
            } else {
                neurogenesis(i, Genome{});
            }
        }
    }
}

inline Genome Core::compress(int idx) {
    Genome g;
    g.w_in.resize(N, 0.0f);
    g.w_out.resize(N, 0.0f);
    for (int j = 0; j < N; ++j) {
        g.w_in[j] = w[j * N + idx];
        g.w_out[j] = w[idx * N + j];
    }
    g.fitness = n[idx].trophic;
    return g;
}

inline void Core::neurogenesis(int idx, const Genome& g) {
    n[idx] = Neuron{};
    n[idx].V = cfg.V_rest;
    n[idx].V_thresh = cfg.V_thresh0;
    n[idx].trophic = 0.8f;
    for (int j = 0; j < N; ++j) {
        e[j * N + idx] = 0.0f;
        e[idx * N + j] = 0.0f;
    }

    std::normal_distribution<float> mut(0.0f, cfg.mutate);
    bool has = !g.w_in.empty();

    for (int j = 0; j < N; ++j) {
        if (j == idx) continue;
        float win = has ? g.w_in[j] * cfg.inherit + mut(rng) : mut(rng);
        float wout = has ? g.w_out[j] * cfg.inherit + mut(rng) : mut(rng);
        w[j * N + idx] = std::clamp(win, cfg.w_min, cfg.w_max);
        w[idx * N + j] = std::clamp(wout, cfg.w_min, cfg.w_max);
    }
}

inline void Core::modulate(float modulation) {
    modulation = std::clamp(modulation, -1.0f, 1.0f);
    // Обучаем ТОЛЬКО hidden -> motor
    for (int i = hid0; i < hid1; ++i) {
        for (int j = mot0; j < mot1; ++j) {
            int idx = i * N + j;
            w[idx] += cfg.lr * e[idx] * modulation;
            w[idx] = std::clamp(w[idx], cfg.w_min, cfg.w_max);
        }
    }
}

inline int Core::motor_spike_count(int window) const {
    int cnt = 0;
    for (int i = mot0; i < mot1; ++i) {
        if (step_counter - n[i].last_spike < window) cnt++;
    }
    return cnt;
}

inline float Core::mean_rate_window(int window) const {
    int cnt = 0;
    for (int i = 0; i < N; ++i) {
        if (step_counter - n[i].last_spike < window) cnt++;
    }
    return static_cast<float>(cnt) / N;
}

inline std::tuple<int,int,int> Core::get_layer_spikes() const {
    int in_spikes = 0, hid_spikes = 0, mot_spikes = 0;
    for (int i = in0; i < in1; ++i) if (n[i].spike) in_spikes++;
    for (int i = hid0; i < hid1; ++i) if (n[i].spike) hid_spikes++;
    for (int i = mot0; i < mot1; ++i) if (n[i].spike) mot_spikes++;
    return {in_spikes, hid_spikes, mot_spikes};
}

inline void Core::stats(int step, int window) const {
    int young = 0;
    for (const auto& nr : n) if (nr.age < cfg.crit_period) young++;
    float trophic_mean = 0.0f;
    for (const auto& nr : n) trophic_mean += nr.trophic;
    trophic_mean /= N;

    std::cout << "[Stats@" << step << "] "
              << "rate=" << mean_rate_window(window)
              << " motor_spikes=" << motor_spike_count(window)
              << " young=" << young
              << " pool=" << pool.size()
              << " trophic=" << trophic_mean
              << "\n";
}

} // namespace nc