#include <SFML/Graphics.hpp>
#include <vector>
#include <random>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cmath>

int main() {
    const int Nside = 20;
    const int N = Nside * Nside;
    const double dt = 0.001;
    const double m = 1.0;
    const double lam = 0.5;
    const unsigned int windowSize = 400;
    const float cellSize = windowSize / float(Nside);

    std::vector<double> phi(N), pi(N, 0.0), dH(N, 0.0);
    std::vector<std::vector<double>> W(N, std::vector<double>(N));

    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(-0.1, 0.1);
    std::uniform_real_distribution<double> wdist(-0.02, 0.02); // Разрешаем отрицательные веса

    // Инициализация симметричной матрицы весов
    for (int i = 0; i < N; i++) {
        phi[i] = dist(rng);
        for (int j = i + 1; j < N; j++) { // Только верхний треугольник
            W[i][j] = wdist(rng);
            W[j][i] = W[i][j]; // Симметрия
        }
        W[i][i] = 0.0; // Нет самосвязей
    }

    auto compute_local_energy = [&](int i) {
        double V = 0.5 * m * m * phi[i] * phi[i] + (lam / 6.0) * phi[i] * phi[i] * phi[i];
        double inter = 0.0;
        for (int j = 0; j < N; j++)
            inter += 0.5 * W[i][j] * (phi[i] - phi[j]) * (phi[i] - phi[j]);
        return 0.5 * pi[i] * pi[i] + V + inter;
    };

    auto classify = [](const std::vector<double>& phi) -> int {
        double sum = 0.0;
        for(double v : phi) sum += v;
        double avg = sum / phi.size();
        if(avg < -0.05) return 0;
        else if(avg < 0.05) return 1;
        else return 2;
    };

    std::ofstream csv("phi_sfml_dump.csv");
    csv << std::fixed << std::setprecision(6);

    sf::RenderWindow window(sf::VideoMode(sf::Vector2(windowSize, windowSize)), "Dark Particle Heatmap");

    int step = 0;
    double min_phi = -1.0, max_phi = 1.0; // Для динамической нормализации

    while (window.isOpen()) {
        // --- обработка событий ---
        while (auto eventOpt = window.pollEvent()) {
            if (eventOpt.has_value()) {
                const auto& event = *eventOpt;

                if (event.is<sf::Event::Closed>())
                    window.close();

                else if (const auto* mousePressed = event.getIf<sf::Event::MouseButtonPressed>()) {
                    int x = mousePressed->position.x;
                    int y = mousePressed->position.y;
                    int cellX = x / cellSize;
                    int cellY = y / cellSize;
                    if (cellX >= 0 && cellX < Nside && cellY >= 0 && cellY < Nside) {
                        int idx = cellY * Nside + cellX;
                        phi[idx] += 0.1; // Уменьшенное воздействие
                    }
                }
            }
        }

        // --- симплектная эволюция φ и π ---
        for(int i = 0; i < N; i++) phi[i] += dt * pi[i];
        
        for(int i = 0; i < N; i++){
            double dV = m * m * phi[i] + 0.5 * lam * phi[i] * phi[i];
            double inter = 0.0;
            for(int j = 0; j < N; j++) 
                inter += W[i][j] * (phi[i] - phi[j]);
            dH[i] = dV + inter;
        }
        
        for(int i = 0; i < N; i++) 
            pi[i] -= dt * dH[i];

        // --- небольшое затухание для стабильности ---
        for(int i = 0; i < N; i++) {
            pi[i] *= 0.999; // Слабое затухание импульса
            // Ограничиваем экстремальные значения
            if(phi[i] > 2.0) phi[i] = 2.0;
            if(phi[i] < -2.0) phi[i] = -2.0;
            if(pi[i] > 10.0) pi[i] = 10.0;
            if(pi[i] < -10.0) pi[i] = -10.0;
        }

        // --- улучшенное обучение (обновление W) ---
        double learning_rate = 0.001; // Уменьшенная скорость обучения
        double weight_decay = 0.999; // Затухание весов
        
        for(int i = 0; i < N; i++) {
            for(int j = i + 1; j < N; j++) { // Только верхний треугольник
                // Правило Хебба с затуханием
                double update = learning_rate * phi[i] * phi[j];
                W[i][j] = W[i][j] * weight_decay + update;
                W[j][i] = W[i][j]; // Сохраняем симметрию
                
                // Ограничение весов в обоих направлениях
                if(W[i][j] > 0.1) W[i][j] = 0.1;
                if(W[i][j] < -0.1) W[i][j] = -0.1;
                W[j][i] = W[i][j];
            }
        }

        // --- вычисление энергии и классификация ---
        double E = 0.0;
        for(int i = 0; i < N; i++) E += compute_local_energy(i);
        E /= N;
        int state = classify(phi);

        // --- обновление динамического диапазона для цветов ---
        double current_min = phi[0], current_max = phi[0];
        for(int i = 1; i < N; i++) {
            if(phi[i] < current_min) current_min = phi[i];
            if(phi[i] > current_max) current_max = phi[i];
        }
        // Плавное обновление диапазона
        min_phi = 0.99 * min_phi + 0.01 * current_min;
        max_phi = 0.99 * max_phi + 0.01 * current_max;
        
        // Гарантируем минимальный диапазон
        if(max_phi - min_phi < 0.1) {
            double center = (min_phi + max_phi) / 2;
            min_phi = center - 0.05;
            max_phi = center + 0.05;
        }

        // --- визуализация с динамической нормализацией ---
        window.clear();
        for(int y = 0; y < Nside; y++) {
            for(int x = 0; x < Nside; x++) {
                int idx = y * Nside + x;
                double norm = (phi[idx] - min_phi) / (max_phi - min_phi);
                if(norm < 0) norm = 0;
                if(norm > 1) norm = 1;
                
                sf::RectangleShape cell(sf::Vector2f(cellSize, cellSize));
                cell.setPosition({x * cellSize, y * cellSize});
                
                // Улучшенная цветовая схема
                sf::Color color;
                if(norm < 0.5) {
                    // От синего к черному для отрицательных значений
                    double intensity = norm * 2.0;
                    color = sf::Color(0, 0, static_cast<unsigned char>(intensity * 255));
                } else {
                    // От черного к красному для положительных значений
                    double intensity = (norm - 0.5) * 2.0;
                    color = sf::Color(static_cast<unsigned char>(intensity * 255), 0, 0);
                }
                
                cell.setFillColor(color);
                window.draw(cell);
            }
        }
        window.display();

        std::cout << "Step " << step << " | Energy: " << E << " | State: " << state 
                  << " | Range: [" << min_phi << ", " << max_phi << "]" << "\r";
        std::cout.flush();

        if(step % 100 == 0) {
            csv << "step_" << step;
            for(double v : phi) csv << "," << v;
            csv << "\n";
        }

        step++;
    }

    csv.close();
    std::cout << "\nSimulation complete.\n";

    return 0;
}