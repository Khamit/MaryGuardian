// core/EmergentCoreImpl.cpp
#include "EmergentCore.hpp"
#include "NeuralGroup.hpp"

void NeuroMemoryRecord::captureFromNeuron(const NeuralGroup& group, int neuron_idx, int step) {
    group_id = 0;
    neuron_id = neuron_idx;
    last_accessed = step;
    
    int size = group.getSize();
    signature.incoming.resize(size);
    signature.outgoing.resize(size);
    
    for (int j = 0; j < size; ++j) {
        signature.incoming[j] = static_cast<float>(group.getWeight(j, neuron_idx));
        signature.outgoing[j] = static_cast<float>(group.getWeight(neuron_idx, j));
    }
    
    signature.firing_rate = static_cast<float>(group.getAverageActivity());
    
    const auto& phi = group.getPhi();
    embedding.assign(phi.begin(), phi.end());
    
    tag = group.getSpecialization();
}