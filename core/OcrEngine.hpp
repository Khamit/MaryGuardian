#pragma once
#include <string>

struct llama_model;
struct llama_context;
struct mtmd_context;

namespace mary {
class OcrEngine {
public:
    ~OcrEngine() { unload(); }
    bool load(const std::string& model_path, const std::string& mmproj_path);
    void unload();
    bool isLoaded() const { return model_ != nullptr; }
    
    // Принимает байты PNG и возвращает распознанный текст
    std::string recognize(const std::string& png_data, int max_tokens = 1500);

private:
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    mtmd_context* mtmd_ = nullptr;
};
} // namespace mary