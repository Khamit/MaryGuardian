#include "core/ModelRuntime.hpp"
#include <llama.h>
#include <common.h>
#include <iostream>
#include <algorithm>
#include <mutex>

namespace mary {

ModelRuntime::ModelRuntime() = default;
ModelRuntime::~ModelRuntime() { unload(); }

void ModelRuntime::unload() {
    std::lock_guard<std::mutex> lk(gen_mu_);   // ждём завершения текущей генерации
    if (ctx_)   { llama_free(ctx_); ctx_ = nullptr; }
    if (model_) { llama_model_free(model_); model_ = nullptr; }
    vocab_ = nullptr;
}

bool ModelRuntime::loadModel(const std::string& path, int n_ctx, int n_gpu_layers) {
    static std::once_flag backend_once;
    std::call_once(backend_once, [] { llama_backend_init(); });

    unload();

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = n_gpu_layers;

    model_ = llama_model_load_from_file(path.c_str(), mparams);
    if (!model_) {
        std::cerr << "[ModelRuntime] Failed to load model: " << path << std::endl;
        return false;
    }
    vocab_ = llama_model_get_vocab(model_);

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = n_ctx;
    cparams.n_batch = 512;
    cparams.n_ubatch = 512;

    ctx_ = llama_init_from_model(model_, cparams);
    if (!ctx_) {
        std::cerr << "[ModelRuntime] Failed to create context" << std::endl;
        llama_model_free(model_);
        model_ = nullptr;
        vocab_ = nullptr;
        return false;
    }

    model_path_ = path;
    n_ctx_ = n_ctx;
    std::cout << "[ModelRuntime] Loaded: " << path << " (ctx=" << n_ctx << ")" << std::endl;
    return true;
}

bool ModelRuntime::isLoaded() const { return model_ != nullptr && ctx_ != nullptr; }

std::vector<int> ModelRuntime::tokenize(const std::string& text, bool add_bos, bool special) {
    if (!vocab_) return {};
    int n_tokens = static_cast<int>(text.length()) + 2;
    std::vector<int> tokens(n_tokens);
    int actual = llama_tokenize(vocab_, text.c_str(), static_cast<int>(text.length()),
                                tokens.data(), n_tokens, add_bos, special);
    if (actual < 0) {
        n_tokens = -actual;
        tokens.resize(n_tokens);
        actual = llama_tokenize(vocab_, text.c_str(), static_cast<int>(text.length()),
                                tokens.data(), n_tokens, add_bos, special);
        if (actual < 0) return {};
    }
    tokens.resize(actual);
    return tokens;
}

std::string ModelRuntime::detokenize(const std::vector<int>& tokens) {
    if (!vocab_) return "";
    std::string result;
    char buf[256];
    for (int tok : tokens) {
        int n = llama_token_to_piece(vocab_, tok, buf, sizeof(buf), 0, true);
        if (n > 0) result.append(buf, n);
    }
    return result;
}

std::string ModelRuntime::doGenerate(const std::string& prompt, const GenerationConfig& config) {
    std::lock_guard<std::mutex> lk(gen_mu_);
    if (!isLoaded()) return "";

    llama_memory_t mem = llama_get_memory(ctx_);
    llama_memory_clear(mem, true);   // полный сброс KV-cache перед каждым запросом

    auto prompt_tokens = tokenize(prompt, true, true);
    if (prompt_tokens.empty()) return "";

    const int n_ctx_total = static_cast<int>(llama_n_ctx(ctx_));
    const int n_batch     = static_cast<int>(llama_n_batch(ctx_));

    const int max_prompt = n_ctx_total - config.max_tokens;
    if (max_prompt <= 0) {
        std::cerr << "[ModelRuntime] ERROR: max_tokens (" << config.max_tokens
                  << ") >= n_ctx (" << n_ctx_total << ")." << std::endl;
        return "";
    }
    if (static_cast<int>(prompt_tokens.size()) > max_prompt) {
        std::cerr << "[ModelRuntime] ERROR: prompt " << prompt_tokens.size()
                  << " tokens > " << max_prompt << ". Refusing to truncate.\n";
        return "";
    }
    const int n_prompt = static_cast<int>(prompt_tokens.size());

    // Порядок важен: grammar → penalties → фильтры → финальный выбор (dist/greedy).
    llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (config.use_grammar && !config.grammar_gbnf.empty()) {
        llama_sampler_chain_add(smpl, llama_sampler_init_grammar(
            vocab_, config.grammar_gbnf.c_str(), "root"));
    }
    // Сигнатура зависит от версии llama.cpp; у вас n_vocab первым аргументом.
    llama_sampler_chain_add(smpl, llama_sampler_init_penalties(
        llama_vocab_n_tokens(vocab_), 64, config.repeat_penalty, 0.0f, 0.0f));
    if (config.temperature <= 0.0f) {
        llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
    } else {
        llama_sampler_chain_add(smpl, llama_sampler_init_top_k(config.top_k));
        llama_sampler_chain_add(smpl, llama_sampler_init_top_p(config.top_p, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(config.temperature));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
    }

    llama_batch batch = llama_batch_init(n_batch, 0, 1);

    for (int i = 0; i < n_prompt; i += n_batch) {
        int n_chunk = std::min(n_batch, n_prompt - i);
        for (int j = 0; j < n_chunk; ++j) {
            batch.token[j]     = prompt_tokens[i + j];
            batch.pos[j]       = i + j;
            batch.n_seq_id[j]  = 1;
            batch.seq_id[j][0] = 0;
            batch.logits[j]    = 0;
        }
        if (i + n_chunk >= n_prompt) batch.logits[n_chunk - 1] = 1;
        batch.n_tokens = n_chunk;

        if (llama_decode(ctx_, batch) != 0) {
            llama_batch_free(batch);
            llama_sampler_free(smpl);
            return "";
        }
    }

    std::vector<int> generated;
    int n_cur = n_prompt;

    for (int i = 0; i < config.max_tokens; ++i) {
        if (n_cur >= n_ctx_total) break;

        llama_token new_token = llama_sampler_sample(smpl, ctx_, -1);   // внутри делает accept
        if (llama_vocab_is_eog(vocab_, new_token)) break;

        generated.push_back(new_token);

        batch.n_tokens     = 1;
        batch.token[0]     = new_token;
        batch.pos[0]       = n_cur;
        batch.n_seq_id[0]  = 1;
        batch.seq_id[0][0] = 0;
        batch.logits[0]    = 1;
        ++n_cur;

        if (llama_decode(ctx_, batch) != 0) break;
    }

    llama_batch_free(batch);
    llama_sampler_free(smpl);
    return detokenize(generated);
}

std::string ModelRuntime::generateText(const std::string& prompt, const GenerationConfig& config) {
    GenerationConfig cfg = config;
    cfg.use_grammar = false;   // Qwen нестабилен с grammar в llama.cpp
    return doGenerate(prompt, cfg);
}

nlohmann::json ModelRuntime::generateStructured(const std::string& prompt,
                                                const std::string& grammar_gbnf,
                                                int max_tokens) {
    (void)grammar_gbnf;

    GenerationConfig cfg;
    cfg.max_tokens = max_tokens;
    cfg.temperature = 0.1f;

    std::string raw = generateText(prompt, cfg);

    const size_t start = raw.find('{');
    const size_t end = raw.rfind('}');
    if (start != std::string::npos && end != std::string::npos && end > start)
        raw = raw.substr(start, end - start + 1);

    try {
        auto j = nlohmann::json::parse(raw);
        if (!j.is_object()) return nlohmann::json{{"raw", raw}, {"error", "json_not_object"}};
        return j;
    } catch (const nlohmann::json::parse_error&) {
        return nlohmann::json{{"raw", raw}, {"error", "json_parse_failed"}};
    }
}

ToolCall ModelRuntime::proposeToolCall(const std::string& prompt, const nlohmann::json& tool_schema) {
    (void)tool_schema;
    nlohmann::json result = generateStructured(prompt, "", 256);

    ToolCall tc;
    if (result.contains("tool_name") && result["tool_name"].is_string())
        tc.tool_name = result["tool_name"].get<std::string>();
    if (result.contains("arguments") && result["arguments"].is_object())
        tc.arguments = result["arguments"];
    return tc;
}

std::string ModelRuntime::getModelId() const { return model_path_; }

int ModelRuntime::countTokens(const std::string& text) {
    return (int)tokenize(text, false, false).size();
}

} // namespace mary