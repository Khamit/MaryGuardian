#pragma once

#include <vector>
#include <deque>
#include <unordered_map>
#include <string>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <cassert>
#include <random>
#include <memory>

// Forward declarations
class NeuralGroup;

// ============================================================================
// НОВАЯ СТРУКТУРА ПАМЯТИ — КОРРЕЛИРУЕТСЯ С НЕЙРОНАМИ
// ============================================================================

/**
 * @struct SynapticSignature
 * @brief "Отпечаток пальца" нейрона — его синаптические веса
 * 
 * Позволяет сравнивать нейроны не по позиции в пространстве,
 * а по их функциональной роли (с кем они связаны).
 */
struct SynapticSignature {
    std::vector<float> incoming;  // веса от других нейронов (пре)
    std::vector<float> outgoing;  // веса к другим нейронам (пост)
    float firing_rate;            // средняя частота спайков
    float spike_timing_pattern;   // грубая характеристика временного паттерна
    
    // Нормализованное косинусное расстояние между двумя сигнатурами
    static float cosineSimilarity(const SynapticSignature& a, const SynapticSignature& b) {
        size_t sz = std::min(a.incoming.size(), b.incoming.size());
        float dot = 0.f, na = 0.f, nb = 0.f;
        
        for (size_t i = 0; i < sz; ++i) {
            dot += a.incoming[i] * b.incoming[i];
            na += a.incoming[i] * a.incoming[i];
            nb += b.incoming[i] * b.incoming[i];
        }
        
        for (size_t i = 0; i < sz; ++i) {
            dot += a.outgoing[i] * b.outgoing[i];
            na += a.outgoing[i] * a.outgoing[i];
            nb += b.outgoing[i] * b.outgoing[i];
        }
        
        // Добавляем частоту спайков
        dot += a.firing_rate * b.firing_rate;
        na += a.firing_rate * a.firing_rate;
        nb += b.firing_rate * b.firing_rate;
        
        float denom = std::sqrt(na) * std::sqrt(nb);
        return (denom < 1e-9f) ? 0.f : std::clamp(dot / denom, 0.f, 1.f);
    }
};

/**
 * @struct NeuroMemoryRecord
 * @brief Запись памяти, коррелирующаяся с состоянием нейронов
 * 
 * В отличие от старой MemoryRecord (которая хранила только активности групп),
 * эта структура хранит полную сигнатуру нейрона, включая синаптические веса.
 */
struct NeuroMemoryRecord {
    // Идентификация
    int group_id;                      // в какой группе был нейрон
    int neuron_id;                     // индекс нейрона в группе
    
    // Сигнатура нейрона (функциональный отпечаток)
    SynapticSignature signature;
    
    // Метаданные
    float importance = 0.5f;           // важность [0,1]
    float decay_rate = 0.01f;          // скорость затухания
    int age = 0;                       // шагов с момента создания
    int last_accessed = 0;             // последний шаг доступа
    float trophic_history = 0.0f;      // история трофических сигналов
    
    // Семантическая информация
    std::string tag;                   // "sensory", "motor", "semantic", etc.
    std::vector<float> embedding;      // эмбеддинг из semantic групп (16-21)
    
    // Статистика
    float avg_firing_rate = 0.0f;      // средняя частота спайков
    float spike_variability = 0.0f;    // вариативность интервалов
    
    // Вычисление релевантности для текущего нейрона
    float relevanceTo(const SynapticSignature& query, int current_step) const {
        float recency = std::exp(-decay_rate * (current_step - last_accessed));
        float sig_sim = SynapticSignature::cosineSimilarity(signature, query);
        return importance * recency * sig_sim;
    }
    // Обновление сигнатуры из живого нейрона
    void captureFromNeuron(const NeuralGroup& group, int neuron_idx, int step);
};

// ============================================================================
// LONG-TERM POTENTIATION CACHE — для быстрого доступа к часто используемым паттернам
// ============================================================================

/**
 * @class LTMCache
 * @brief Кэш для LTM, ускоряющий поиск релевантных паттернов
 * 
 * Хранит предвычисленные эмбеддинги для быстрого косинусного сравнения.
 */
class LTMCache {
public:
    struct CachedEntry {
        int record_id;
        std::vector<float> embedding;
        float importance;
    };
    
    void rebuild(const std::deque<NeuroMemoryRecord>& ltm) {
        cache_.clear();
        id_to_idx_.clear();
        
        for (size_t i = 0; i < ltm.size(); ++i) {
            CachedEntry entry;
            entry.record_id = i;
            entry.embedding = ltm[i].embedding;
            entry.importance = ltm[i].importance;
            cache_.push_back(entry);
            id_to_idx_[i] = i;
        }
    }
    
    std::vector<int> findSimilar(const std::vector<float>& query_embedding, 
                                  int top_k, float min_similarity = 0.5f) {
        std::vector<std::pair<float, int>> scored;
        for (size_t i = 0; i < cache_.size(); ++i) {
            float sim = cosineSimilarity(query_embedding, cache_[i].embedding);
            if (sim > min_similarity) {
                scored.push_back({sim, i});
            }
        }
        
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        
        std::vector<int> result;
        for (int i = 0; i < top_k && i < (int)scored.size(); ++i) {
            result.push_back(cache_[scored[i].second].record_id);
        }
        return result;
    }
    
private:
    std::vector<CachedEntry> cache_;
    std::unordered_map<int, int> id_to_idx_;
    
    static float cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
        size_t sz = std::min(a.size(), b.size());
        float dot = 0.f, na = 0.f, nb = 0.f;
        for (size_t i = 0; i < sz; ++i) {
            dot += a[i] * b[i];
            na += a[i] * a[i];
            nb += b[i] * b[i];
        }
        float denom = std::sqrt(na) * std::sqrt(nb);
        return (denom < 1e-9f) ? 0.f : std::clamp(dot / denom, 0.f, 1.f);
    }
};

// ============================================================================
// EMERGENT MEMORY — НОВАЯ ВЕРСИЯ С СИГНАТУРАМИ НЕЙРОНОВ
// ============================================================================

class EmergentMemory {
public:
    struct Config {
        size_t stm_capacity = 256;           // максимальный размер STM
        size_t ltm_capacity = 2048;          // максимальный размер LTM
        
        // Пороги консолидации
        float consolidation_threshold = 0.4f; // min importance для STM→LTM
        float discard_threshold = 0.03f;       // LTM элементы ниже этого удаляются
        float similarity_merge = 0.85f;        // выше этого — объединять
        
        // Временные параметры
        int min_age_for_ltm = 50;              // сколько шагов STM элемент должен прожить
        float stm_decay = 0.03f;               // затухание важности в STM
        float ltm_decay = 0.001f;              // затухание важности в LTM
        
        // Для трофической регуляции
        float trophic_boost = 0.2f;            // насколько трофины увеличивают важность
    };
    
    Config cfg;
    
    EmergentMemory() = default;
    explicit EmergentMemory(Config c) : cfg(std::move(c)) {}
    
    // ===== ЗАПИСЬ В STM (из состояния нейрона) =====
    void writeSTM(const NeuroMemoryRecord& record, int step) {
        // Проверяем, есть ли похожий элемент
        for (auto& r : stm_) {
            float sim = SynapticSignature::cosineSimilarity(r.signature, record.signature);
            if (sim > cfg.similarity_merge) {
                // Объединяем: усредняем веса, повышаем важность
                mergeRecords(r, record);
                r.last_accessed = step;
                r.importance = std::clamp(r.importance + 0.1f, 0.f, 1.f);
                return;
            }
        }
        
        stm_.push_back(record);
        enforceSTMCapacity();
    }
    
    // ===== ЗАПИСЬ ИЗ ВЕКТОРА АКТИВНОСТЕЙ (для обратной совместимости) =====
    void writeSTM(const std::vector<float>& pattern,
                  float importance,
                  float entropy,
                  const std::string& tag = "",
                  int step = 0) {
        // Создаём "пустую" запись с эмбеддингом из pattern
        NeuroMemoryRecord rec;
        rec.embedding = pattern;
        rec.importance = importance;
        rec.tag = tag;
        rec.last_accessed = step;
        
        // Для совместимости — инициализируем сигнатуру нулями
        rec.signature.incoming.resize(pattern.size(), 0.f);
        rec.signature.outgoing.resize(pattern.size(), 0.f);
        rec.signature.firing_rate = 0.f;
        
        stm_.push_back(rec);
        enforceSTMCapacity();
    }
    
    // ===== ЗАПРОС: найти top-k релевантных записей =====
    std::vector<const NeuroMemoryRecord*> query(const SynapticSignature& query,
                                                int top_k,
                                                int current_step) const {
        std::vector<std::pair<float, const NeuroMemoryRecord*>> scored;
        
        for (const auto& r : stm_) {
            scored.push_back({r.relevanceTo(query, current_step), &r});
        }
        for (const auto& r : ltm_) {
            scored.push_back({r.relevanceTo(query, current_step), &r});
        }
        
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        
        std::vector<const NeuroMemoryRecord*> out;
        for (int i = 0; i < top_k && i < (int)scored.size(); ++i) {
            out.push_back(scored[i].second);
        }
        return out;
    }
    
    // ===== ЗАПРОС ПО ЭМБЕДДИНГУ (для быстрого поиска) =====
    std::vector<const NeuroMemoryRecord*> queryByEmbedding(const std::vector<float>& embedding,
                                                           int top_k,
                                                           int current_step) const {
        std::vector<std::pair<float, const NeuroMemoryRecord*>> scored;
        
        for (const auto& r : stm_) {
            float sim = cosineSimilarity(embedding, r.embedding);
            scored.push_back({sim * r.importance, &r});
        }
        for (const auto& r : ltm_) {
            float sim = cosineSimilarity(embedding, r.embedding);
            scored.push_back({sim * r.importance, &r});
        }
        
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        
        std::vector<const NeuroMemoryRecord*> out;
        for (int i = 0; i < top_k && i < (int)scored.size(); ++i) {
            out.push_back(scored[i].second);
        }
        return out;
    }
    
    // ===== ШАГ: затухание, консолидация, прунинг =====
    std::pair<int, int> step(int current_step) {
        int consolidated = 0, discarded = 0;
        
        // 1. Затухание STM
        for (auto& r : stm_) {
            r.age++;
            r.importance *= (1.f - cfg.stm_decay);
            r.trophic_history *= 0.99f;
        }
        
        // 2. Консолидация STM → LTM
        std::deque<NeuroMemoryRecord> remaining_stm;
        for (auto& r : stm_) {
            bool old_enough = (r.age >= cfg.min_age_for_ltm);
            bool important = (r.importance >= cfg.consolidation_threshold);
            bool has_trophic = (r.trophic_history > 0.1f);
            
            if ((old_enough && important) || has_trophic) {
                consolidateToLTM(std::move(r), current_step);
                ++consolidated;
            } else if (r.importance > cfg.discard_threshold) {
                remaining_stm.push_back(std::move(r));
            } else {
                ++discarded;
            }
        }
        stm_ = std::move(remaining_stm);
        
        // 3. Затухание LTM
        for (auto& r : ltm_) {
            r.age++;
            r.importance *= (1.f - cfg.ltm_decay);
            r.trophic_history *= 0.995f;
        }
        
        // 4. Прунинг LTM
        size_t before = ltm_.size();
        ltm_.erase(std::remove_if(ltm_.begin(), ltm_.end(),
            [&](const NeuroMemoryRecord& r) {
                return r.importance < cfg.discard_threshold;
            }), ltm_.end());
        discarded += (int)(before - ltm_.size());
        
        // 5. Ограничение ёмкости
        enforceLTMCapacity();
        
        // 6. Обновляем кэш, если LTM изменился значительно
        if (consolidated > 0 || discarded > 0) {
            ltm_cache_.rebuild(ltm_);
        }
        
        return {consolidated, discarded};
    }
    
    // ===== ПОДКРЕПЛЕНИЕ: повышаем важность похожих записей =====
    void reinforce(const SynapticSignature& query, float boost, int step) {
        for (auto& r : stm_) {
            float sim = SynapticSignature::cosineSimilarity(r.signature, query);
            if (sim > 0.5f) {
                r.importance = std::clamp(r.importance + boost * sim, 0.f, 1.f);
                r.last_accessed = step;
            }
        }
        for (auto& r : ltm_) {
            float sim = SynapticSignature::cosineSimilarity(r.signature, query);
            if (sim > 0.5f) {
                r.importance = std::clamp(r.importance + boost * sim * 0.5f, 0.f, 1.f);
                r.last_accessed = step;
            }
        }
    }
    
    // ===== ТРОФИЧЕСКОЕ ПОДКРЕПЛЕНИЕ (от выживших нейронов) =====
    void trophicReinforce(int neuron_id, int group_id, float trophic_signal, int step) {
        for (auto& r : stm_) {
            if (r.neuron_id == neuron_id && r.group_id == group_id) {
                r.trophic_history += trophic_signal;
                r.importance = std::clamp(r.importance + trophic_signal * cfg.trophic_boost, 0.f, 1.f);
                r.last_accessed = step;
                break;
            }
        }
        for (auto& r : ltm_) {
            if (r.neuron_id == neuron_id && r.group_id == group_id) {
                r.trophic_history += trophic_signal;
                r.importance = std::clamp(r.importance + trophic_signal * cfg.trophic_boost * 0.5f, 0.f, 1.f);
                r.last_accessed = step;
                break;
            }
        }
    }
    
    // ===== ОСЛАБЛЕНИЕ (для отрицательного подкрепления) =====
    void weaken(const SynapticSignature& query, float penalty) {
        for (auto& r : ltm_) {
            float sim = SynapticSignature::cosineSimilarity(r.signature, query);
            if (sim > 0.7f) {
                r.importance = std::clamp(r.importance - penalty * sim, 0.f, 1.f);
            }
        }
    }
    
    // ===== СТАТИСТИКА =====
    size_t stmSize() const { return stm_.size(); }
    size_t ltmSize() const { return ltm_.size(); }
    
    float averageSTMImportance() const { return averageImportance(stm_); }
    float averageLTMImportance() const { return averageImportance(ltm_); }
    
    const std::deque<NeuroMemoryRecord>& getLTM() const { return ltm_; }
    const std::deque<NeuroMemoryRecord>& getSTM() const { return stm_; }
    const LTMCache& getCache() const { return ltm_cache_; }
    
    // ===== ПОИСК ПО ТЕГУ =====
    std::vector<const NeuroMemoryRecord*> findByTag(const std::string& tag) const {
        std::vector<const NeuroMemoryRecord*> result;
        for (const auto& r : ltm_) {
            if (r.tag == tag) result.push_back(&r);
        }
        return result;
    }
    
    // ===== ПОЛУЧЕНИЕ КОНТЕКСТА ДЛЯ ДЕЙСТВИЙ =====
    std::vector<std::vector<float>> getContextPatterns(int top_k, const std::vector<float>& current_embedding) const {
        auto matches = queryByEmbedding(current_embedding, top_k, 0);
        std::vector<std::vector<float>> contexts;
        for (const auto* m : matches) {
            if (!m->embedding.empty()) {
                contexts.push_back(m->embedding);
            }
        }
        return contexts;
    }
    
private:
    std::deque<NeuroMemoryRecord> stm_;
    std::deque<NeuroMemoryRecord> ltm_;
    LTMCache ltm_cache_;
    
    void enforceSTMCapacity() {
        while (stm_.size() > cfg.stm_capacity) {
            auto it = std::min_element(stm_.begin(), stm_.end(),
                [](const NeuroMemoryRecord& a, const NeuroMemoryRecord& b) {
                    return a.importance < b.importance;
                });
            stm_.erase(it);
        }
    }
    
    void enforceLTMCapacity() {
        while (ltm_.size() > cfg.ltm_capacity) {
            auto it = std::min_element(ltm_.begin(), ltm_.end(),
                [](const NeuroMemoryRecord& a, const NeuroMemoryRecord& b) {
                    return a.importance < b.importance;
                });
            ltm_.erase(it);
        }
    }
    
    void consolidateToLTM(NeuroMemoryRecord rec, int step) {
        rec.last_accessed = step;
        rec.decay_rate = cfg.ltm_decay;
        
        // Проверяем на слияние с существующим LTM
        for (auto& r : ltm_) {
            float sim = SynapticSignature::cosineSimilarity(r.signature, rec.signature);
            if (sim > cfg.similarity_merge) {
                mergeRecords(r, rec);
                r.last_accessed = step;
                r.importance = std::clamp(r.importance + rec.importance * 0.3f, 0.f, 1.f);
                return;
            }
        }
        
        ltm_.push_back(std::move(rec));
    }
    
    void mergeRecords(NeuroMemoryRecord& target, const NeuroMemoryRecord& source) {
        float w1 = target.importance, w2 = source.importance;
        float total = w1 + w2 + 1e-9f;
        
        // Усредняем сигнатуры
        for (size_t i = 0; i < std::min(target.signature.incoming.size(), source.signature.incoming.size()); ++i) {
            target.signature.incoming[i] = (target.signature.incoming[i] * w1 + source.signature.incoming[i] * w2) / total;
            target.signature.outgoing[i] = (target.signature.outgoing[i] * w1 + source.signature.outgoing[i] * w2) / total;
        }
        
        // Усредняем эмбеддинги
        for (size_t i = 0; i < std::min(target.embedding.size(), source.embedding.size()); ++i) {
            target.embedding[i] = (target.embedding[i] * w1 + source.embedding[i] * w2) / total;
        }
        
        target.signature.firing_rate = (target.signature.firing_rate * w1 + source.signature.firing_rate * w2) / total;
        target.trophic_history = (target.trophic_history * w1 + source.trophic_history * w2) / total;
    }
    
    float averageImportance(const std::deque<NeuroMemoryRecord>& pool) const {
        if (pool.empty()) return 0.f;
        float sum = 0.f;
        for (const auto& r : pool) sum += r.importance;
        return sum / pool.size();
    }
    
    static float cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
        size_t sz = std::min(a.size(), b.size());
        float dot = 0.f, na = 0.f, nb = 0.f;
        for (size_t i = 0; i < sz; ++i) {
            dot += a[i] * b[i];
            na += a[i] * a[i];
            nb += b[i] * b[i];
        }
        float denom = std::sqrt(na) * std::sqrt(nb);
        return (denom < 1e-9f) ? 0.f : std::clamp(dot / denom, 0.f, 1.f);
    }
};

// ============================================================================
// PREDICTION UNIT — С УЧЁТОМ СИГНАТУР НЕЙРОНОВ
// ============================================================================

class PredictionUnit {
public:
    static constexpr int N = 32; // NUM_GROUPS
    
    PredictionUnit() {
        weights_.assign(N * N, 0.f);
        bias_.assign(N, 0.f);
        prev_state_.assign(N, 0.5f);
    }
    
    // Предсказание на основе групповых активностей
    std::vector<float> step(const std::vector<float>& current_state) {
        assert((int)current_state.size() == N);
        
        std::vector<float> predicted(N, 0.f);
        for (int j = 0; j < N; ++j) {
            float val = bias_[j];
            for (int i = 0; i < N; ++i) {
                val += weights_[j * N + i] * prev_state_[i];
            }
            predicted[j] = std::tanh(val);
        }
        
        // Ошибка
        std::vector<float> error(N);
        float total_error = 0.f;
        for (int j = 0; j < N; ++j) {
            float e = current_state[j] - predicted[j];
            error[j] = std::abs(e);
            total_error += error[j];
        }
        last_total_error_ = total_error / N;
        
        // Online обучение (только если есть предыдущее состояние)
        if (step_count_ > 0) {
            float lr = 0.01f;
            for (int j = 0; j < N; ++j) {
                float delta = current_state[j] - predicted[j];
                bias_[j] += lr * delta;
                for (int i = 0; i < N; ++i) {
                    weights_[j * N + i] += lr * delta * prev_state_[i];
                }
            }
        }
        
        prev_state_ = current_state;
        ++step_count_;
        
        // Награда: высокая если ошибка мала
        std::vector<float> reward(N);
        for (int j = 0; j < N; ++j) {
            reward[j] = std::exp(-error[j] * 5.f);
        }
        return reward;
    }
    
    float getLastError() const { return last_total_error_; }
    float getSurprise() const { return std::tanh(last_total_error_ * 3.f); }
    
private:
    std::vector<float> weights_;
    std::vector<float> bias_;
    std::vector<float> prev_state_;
    float last_total_error_ = 0.f;
    int step_count_ = 0;
};

// ============================================================================
// SELF EVALUATOR — СРАВНЕНИЕ С ЭТАЛОНАМИ
// ============================================================================

class SelfEvaluator {
public:
    struct GoodPattern {
        std::vector<float> state;
        float score;
        int step;
    };
    
    static constexpr int HALL_SIZE = 32;
    
    float evaluate(const std::vector<float>& state, float external_reward, int step) {
        float internal_score = computeInternalScore(state);
        float combined = std::clamp(0.7f * external_reward + 0.3f * internal_score, 0.f, 1.f);
        
        // Обновляем зал славы
        if (combined > 0.6f) {
            hall_.push_back({state, combined, step});
            if ((int)hall_.size() > HALL_SIZE) {
                auto it = std::min_element(hall_.begin(), hall_.end(),
                    [](const GoodPattern& a, const GoodPattern& b) { return a.score < b.score; });
                hall_.erase(it);
            }
        }
        
        score_history_.push_back(combined);
        if (score_history_.size() > 200) score_history_.pop_front();
        
        return combined;
    }
    
    float improvementTrend(int window = 50) const {
        if ((int)score_history_.size() < window * 2) return 0.f;
        float recent = 0.f, old = 0.f;
        int n = (int)score_history_.size();
        for (int i = n - window; i < n; ++i) recent += score_history_[i];
        for (int i = n - window * 2; i < n - window; ++i) old += score_history_[i];
        return (recent - old) / window;
    }
    
    float bestScore() const {
        if (hall_.empty()) return 0.f;
        return std::max_element(hall_.begin(), hall_.end(),
            [](const GoodPattern& a, const GoodPattern& b) { return a.score < b.score; })->score;
    }
    
    const std::vector<GoodPattern>& getHall() const { return hall_; }
    
private:
    std::vector<GoodPattern> hall_;
    std::deque<float> score_history_;
    
    float computeInternalScore(const std::vector<float>& state) const {
        if (hall_.empty()) return 0.5f;
        
        float best_sim = 0.f;
        for (const auto& g : hall_) {
            float dot = 0.f, na = 0.f, nb = 0.f;
            for (size_t i = 0; i < std::min(state.size(), g.state.size()); ++i) {
                dot += state[i] * g.state[i];
                na += state[i] * state[i];
                nb += g.state[i] * g.state[i];
            }
            float sim = (na > 0 && nb > 0) ? dot / (std::sqrt(na) * std::sqrt(nb)) : 0.f;
            best_sim = std::max(best_sim, sim * g.score);
        }
        return std::clamp(best_sim, 0.f, 1.f);
    }
};

// ============================================================================
// EMERGENT CONTROLLER — НОВАЯ ВЕРСИЯ
// ============================================================================

struct EmergentSignal {
    std::vector<float> per_group_reward;  // размер NUM_GROUPS
    float surprise;                        // [0,1] неожиданность
    float quality;                         // [0,1] качество текущего состояния
    float temperature_delta;               // изменение температуры внимания
    float consolidation_pressure;          // [0,1] срочность консолидации
    bool should_explore;                   // режим исследования
    bool should_consolidate;               // режим консолидации
    int ltm_size;
    int stm_size;
    float improvement_trend;
    
    // Дополнительные поля для нейрон-ориентированной памяти
    std::vector<float> context_embedding;  // релевантный контекст из LTM
    int neurons_consolidated = 0;
    int neurons_discarded = 0;

    // риск галлюцинации на основе Lagrangian аудита
    float hallucination_risk = 0.0f;
    
    // сохранённая энергия системы
    double system_energy = 0.0;
};
// EmergentCore.hpp - оставить только объявление метода

class EmergentController {
public:
    EmergentMemory memory;
    PredictionUnit predictor;
    SelfEvaluator evaluator;
    
    struct Config {
        float surprise_explore_threshold = 0.4f;
        float surprise_consolidate_threshold = 0.15f;
        float quality_consolidate_threshold = 0.5f;
        float temperature_explore_boost = 0.05f;
        float temperature_exploit_decay = 0.03f;
        float context_retrieval_k = 5;
    } cfg;
    
    EmergentController();
    
    // Только объявления, без реализации
    EmergentSignal tick(const std::vector<float>& group_averages,
                        const std::vector<NeuralGroup>& groups,
                        float external_reward,
                        int step);
    
    EmergentSignal tick(const std::vector<float>& group_averages,
                        float external_reward,
                        int step);
    
    std::vector<const NeuroMemoryRecord*> queryContext(const std::vector<float>& state, int top_k) const;
    std::vector<const NeuroMemoryRecord*> getPatternsByTag(const std::string& tag) const;
    
private:
    static float computeEntropy(const std::vector<float>& v);
};