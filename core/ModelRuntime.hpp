#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>

struct llama_model;
struct llama_context;
struct llama_sampler;
struct llama_vocab;

namespace mary {

struct GenerationConfig {
    int max_tokens = 512;
    float temperature = 0.7f;   // <= 0 → greedy
    float top_p = 0.9f;
    int top_k = 40;
    float repeat_penalty = 1.1f;
    bool use_grammar = false;
    std::string grammar_gbnf;
};

struct ToolCall {
    std::string tool_name;
    nlohmann::json arguments;
    float confidence = 0.0f;
};

class ModelRuntime {
public:
    ModelRuntime();
    ~ModelRuntime();

    bool loadModel(const std::string& path, int n_ctx = 8192, int n_gpu_layers = 99);
    bool isLoaded() const;
    void unload();

    // Потокобезопасно: генерации сериализуются через gen_mu_ (HTTP-поток и индексатор).
    std::string generateText(const std::string& prompt, const GenerationConfig& config);
    nlohmann::json generateStructured(const std::string& prompt, const std::string& grammar_gbnf, int max_tokens = 512);
    ToolCall proposeToolCall(const std::string& prompt, const nlohmann::json& tool_schema);

    std::string getModelId() const;
    int getContextSize() const { return n_ctx_; }
    int countTokens(const std::string& text);

private:
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    const llama_vocab* vocab_ = nullptr;
    std::string model_path_;
    int n_ctx_ = 8192;
    std::mutex gen_mu_;

    std::string doGenerate(const std::string& prompt, const GenerationConfig& config);
    std::vector<int> tokenize(const std::string& text, bool add_bos, bool special);
    std::string detokenize(const std::vector<int>& tokens);
};

} // namespace mary