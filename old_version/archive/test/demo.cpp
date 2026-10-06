// demo.cpp — правильная версия
#include "NeuralCore.hpp"
#include <iostream>
#include <vector>

int main() {
    nc::Config cfg;
    cfg.N = 256;
    cfg.n_in = 32;
    cfg.n_motor = 32;
    cfg.lr = 0.01f;
    cfg.el_decay = 0.95f;
    cfg.I_bias_hidden = 1.10f;
    cfg.I_bias_motor  = 1.00f;
    cfg.I_noise = 0.25f;
    cfg.A_plus = 0.01f;
    cfg.A_minus = 0.0105f;
    cfg.V_thresh0 = -52.0f;
    cfg.refractory = 1;
    cfg.exploration_boost = 2.0f;
    cfg.exploration_episodes = 2000;

    nc::Core net(cfg);

    const int episodes = 10000;
    const int episode_len = 100;
    float success_ema = 0.0f;

    for (int ep = 0; ep < episodes; ++ep) {
        net.set_episode(ep);
        
        int motor_during_input = 0;   // Спайки моторов ПОКА вход активен
        int motor_after_input = 0;    // Спайки моторов ПОСЛЕ входа (5-50 шагов)
        int input_spikes_total = 0;   // Для диагностики

        for (int s = 0; s < episode_len; ++s) {
            if (s < 10) {
                std::vector<float> inp(32, 0.0f);
                for (int k = 0; k < 16; ++k) inp[k] = 6.0f;
                net.set_input(inp);
            } else {
                net.clear_input();
            }
            net.step(ep * episode_len + s);

            auto [in_sp, hid_sp, mot_sp] = net.get_layer_spikes();
            input_spikes_total += in_sp;

            if (s < 10) {
                motor_during_input += mot_sp;  // Спайки во время входа
            } else if (s >= 10 && s < 50) {
                motor_after_input += mot_sp;   // Спайки после входа (задержка)
            }
        }

        // Награда ТОЛЬКО если моторы спайковали ПОСЛЕ входа
        // Это значит hidden слой передал сигнал
        float mod = (motor_after_input > 0) ? +1.0f : 0.0f;
        net.modulate(mod);

        success_ema = 0.995f * success_ema + 0.005f * (mod > 0 ? 1.0f : 0.0f);

        if (ep % 1000 == 0) {
            std::cout << "=== Episode " << ep << " ===\n";
            std::cout << "  motor_during_input=" << motor_during_input 
                      << " motor_after_input=" << motor_after_input << "\n";
            std::cout << "  success_rate=" << success_ema << "\n";
            net.stats(ep * episode_len, 20);
            std::cout << "\n";
        }
    }

    std::cout << "Final success rate: " << success_ema << "\n";
    return 0;
}