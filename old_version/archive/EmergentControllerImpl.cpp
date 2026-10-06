// core/EmergentControllerImpl.cpp
#include "EmergentCore.hpp"
#include "NeuralGroup.hpp"
#include <iostream>

EmergentController::EmergentController() {
    EmergentMemory::Config mc;
    mc.stm_capacity = 256;
    mc.ltm_capacity = 2048;
    mc.consolidation_threshold = 0.25f;
    mc.discard_threshold = 0.04f;
    memory = EmergentMemory(mc);
}

EmergentSignal EmergentController::tick(const std::vector<float>& group_averages,
                                        const std::vector<NeuralGroup>& groups,
                                        float external_reward,
                                        int step) {
    EmergentSignal sig;
    
    // 1. Предсказание → per-group награда
    sig.per_group_reward = predictor.step(group_averages);
    sig.surprise = predictor.getSurprise();
    
    // 2. Сбор сигнатур нейронов и запись в STM
    float total_importance = 0.f;
    int neurons_sampled = 0;
    
    for (size_t g = 0; g < groups.size(); ++g) {
        const auto& group = groups[g];
        
        // Семплируем наиболее активные нейроны
        int sample_size = (std::min)(8, group.getSize());
        std::vector<std::pair<float, int>> active_neurons;
        
        const auto& phi = group.getPhi();
        for (int i = 0; i < group.getSize(); ++i) {
            if (phi[i] > 0.3f) {
                active_neurons.push_back({phi[i], i});
            }
        }
        
        std::sort(active_neurons.begin(), active_neurons.end(),
                  std::greater<>());
        
        for (int s = 0; s < sample_size && s < (int)active_neurons.size(); ++s) {
            int neuron_idx = active_neurons[s].second;
            
            NeuroMemoryRecord record;
            record.group_id = g;
            record.neuron_id = neuron_idx;
            record.captureFromNeuron(group, neuron_idx, step);
            
            float neuron_importance = 0.3f;
            neuron_importance += 0.4f * external_reward;
            neuron_importance += 0.3f * (1.f - sig.surprise);
            neuron_importance = std::clamp(neuron_importance, 0.f, 1.f);
            
            record.importance = neuron_importance;
            record.trophic_history = group.getTrophicAccumulator(neuron_idx);
            
            memory.writeSTM(record, step);
            total_importance += neuron_importance;
            neurons_sampled++;
        }
    }
    
    // 3. Самооценка
    float blended_reward = std::max(external_reward, 1.f - sig.surprise);
    sig.quality = evaluator.evaluate(group_averages, blended_reward, step);
    
    // 4. Gating
    sig.should_explore = (sig.surprise > cfg.surprise_explore_threshold);
    sig.should_consolidate = (sig.surprise < cfg.surprise_consolidate_threshold &&
                              sig.quality > cfg.quality_consolidate_threshold);
    
    // 5. Температура
    if (sig.should_explore) {
        sig.temperature_delta = +cfg.temperature_explore_boost;
    } else if (sig.should_consolidate) {
        sig.temperature_delta = -cfg.temperature_exploit_decay;
    } else {
        sig.temperature_delta = 0.f;
    }
    
    // 6. Consolidation pressure
    sig.consolidation_pressure = sig.should_consolidate ? sig.quality : 0.f;
    
    // 7. Шаг памяти
    auto [cons, disc] = memory.step(step);
    sig.neurons_consolidated = cons;
    sig.neurons_discarded = disc;
    
    // 8. Извлечение контекста
    std::vector<float> current_embedding;
    if (!groups.empty() && groups[0].getSize() > 0) {
        const auto& phi = groups[0].getPhi();
        current_embedding.assign(phi.begin(), phi.end());
    }
    
    auto contexts = memory.getContextPatterns(cfg.context_retrieval_k, current_embedding);
    if (!contexts.empty()) {
        sig.context_embedding = contexts[0];
    }
    
    // 9. Трофическое подкрепление
    if (sig.quality > 0.6f) {
        for (size_t g = 0; g < groups.size() && g < 4; ++g) {
            const auto& group = groups[g];
            const auto& phi = group.getPhi();
            for (int i = 0; i < group.getSize(); ++i) {
                if (phi[i] > 0.5f) {
                    memory.trophicReinforce(i, g, sig.quality * 0.1f, step);
                }
            }
        }
    }
    
    // 10. Статистика
    sig.ltm_size = (int)memory.ltmSize();
    sig.stm_size = (int)memory.stmSize();
    sig.improvement_trend = evaluator.improvementTrend();
    
    if ((cons > 0 || disc > 0) && step % 500 == 0) {
        std::cout << "[Emergent] step=" << step
                  << " STM=" << memory.stmSize()
                  << " LTM=" << memory.ltmSize()
                  << " cons=" << cons
                  << " disc=" << disc
                  << " surprise=" << sig.surprise
                  << " quality=" << sig.quality
                  << " temp_delta=" << sig.temperature_delta
                  << std::endl;
    }
    
    return sig;
}

EmergentSignal EmergentController::tick(const std::vector<float>& group_averages,
                                        float external_reward,
                                        int step) {
    std::vector<NeuralGroup> empty_groups;
    return tick(group_averages, empty_groups, external_reward, step);
}

std::vector<const NeuroMemoryRecord*> EmergentController::queryContext(const std::vector<float>& state, int top_k) const {
    return memory.queryByEmbedding(state, top_k, 0);
}

std::vector<const NeuroMemoryRecord*> EmergentController::getPatternsByTag(const std::string& tag) const {
    return memory.findByTag(tag);
}

float EmergentController::computeEntropy(const std::vector<float>& v) {
    float H = 0.f, total = 0.f;
    for (float x : v) total += x;
    if (total < 1e-9f) return 0.f;
    for (float x : v) {
        float p = x / total;
        if (p > 1e-9f) H -= p * std::log2(p);
    }
    return std::clamp(H / std::log2((float)v.size()), 0.f, 1.f);
}