#include "OcrEngine.hpp"
#include "TextUtil.hpp"
#include <llama.h>
#include <mtmd.h>
#include <mtmd-helper.h>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <regex>

namespace mary {

bool OcrEngine::load(const std::string& model_path, const std::string& mmproj_path) {
    unload();
    
    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 99;
    model_ = llama_model_load_from_file(model_path.c_str(), mp);
    if (!model_) {
        std::cerr << "[OcrEngine] Failed to load model: " << model_path << "\n";
        return false;
    }

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 8192; 
    cp.n_batch = 1024; 
    cp.n_ubatch = 1024;
    ctx_ = llama_init_from_model(model_, cp);
    if (!ctx_) { 
        std::cerr << "[OcrEngine] Failed to create context\n";
        unload(); 
        return false; 
    }

    mtmd_context_params tp = mtmd_context_params_default();
    tp.use_gpu = true; 
    tp.n_threads = 4;
    // Явно задаём предсказуемый маркер, чтобы не зависеть от рандомизации llama.cpp
    tp.media_marker = "<__media__>";
    
    mtmd_ = mtmd_init_from_file(mmproj_path.c_str(), model_, tp);
    if (!mtmd_) { 
        std::cerr << "[OcrEngine] Failed to load mmproj: " << mmproj_path << "\n";
        unload(); 
        return false; 
    }
    
    std::cout << "[OcrEngine] Loaded successfully: " << model_path << "\n";
    return true;
}

void OcrEngine::unload() {
    if (mtmd_)  { mtmd_free(mtmd_); mtmd_ = nullptr; }
    if (ctx_)   { llama_free(ctx_); ctx_ = nullptr; }
    if (model_) { llama_model_free(model_); model_ = nullptr; }
}

std::string OcrEngine::recognize(const std::string& png_data, int max_tokens) {
    if (!isLoaded()) {
        std::cerr << "[OcrEngine] Engine not loaded\n";
        return "";
    }
    
    // Очищаем KV-cache
    llama_memory_clear(llama_get_memory(ctx_), true);

    // Загружаем изображение через helper (внутри использует stb_image)
    mtmd_helper_bitmap_wrapper bm = mtmd_helper_bitmap_init_from_buf(
        mtmd_,
        reinterpret_cast<const unsigned char*>(png_data.data()),
        png_data.size(),
        /*placeholder=*/false
    );

    if (!bm.bitmap) {
        std::cerr << "[OcrEngine] Failed to decode image from buffer\n";
        return "";
    }
    
    std::cout << "[OcrEngine] Image loaded: " << mtmd_bitmap_get_nx(bm.bitmap) 
              << "x" << mtmd_bitmap_get_ny(bm.bitmap) << "\n";

    // Получаем актуальный маркер из контекста
    const char* marker = mtmd_get_marker(mtmd_);
    if (!marker) {
        // fallback на дефолтный
        marker = mtmd_default_marker();
    }
    std::cout << "[OcrEngine] Using marker: '" << marker << "'\n";
    
    // Промпт для краткой классификации документа (до 300 символов в UI)
    // OvisOCR2 обучен на Qwen3.5 с ChatML-шаблоном
    std::string prompt = 
        "<|im_start|>user\n" +
        std::string(marker) + "\n"
        "Read this document image and provide a short summary (max 300 characters) "
        "describing: 1) document type (invoice, letter, exam, report, etc.), "
        "2) main topic or purpose, 3) any key entities (companies, people, amounts). "
        "Output only the summary, no explanations."
        "<|im_end|>\n"
        "<|im_start|>assistant\n";
    
    std::cout << "[OcrEngine] Prompt length: " << prompt.length() << " chars\n";
        
    //  КРИТИЧЕСКОЕ ИСПРАВЛЕНИЕ: структура mtmd_input_text имеет 4 поля:
    //   const char * text;
    //   size_t text_len;   ← ЭТО ПОЛЕ Я РАНЬШЕ ПРОПУСКАЛ!
    //   bool add_special;
    //   bool parse_special;
    //
    // Без text_len компилятор интерпретировал 'false' как text_len=0,
    // и токенизатор видел пустую строку, не находя маркер.
    mtmd_input_text text{
        prompt.c_str(),     // text
        prompt.size(),      // text_len  ← ВАЖНО!
        false,
    };
    
    mtmd_input_chunks* chunks = mtmd_input_chunks_init();
    const mtmd_bitmap* bitmaps[] = {bm.bitmap};
    std::string result;

    // Токенизация
    int32_t tokenize_status = mtmd_tokenize(mtmd_, chunks, &text, bitmaps, 1);
    if (tokenize_status != 0) {
        std::cerr << "[OcrEngine] mtmd_tokenize failed with code: " << tokenize_status << "\n";
        mtmd_input_chunks_free(chunks);
        mtmd_bitmap_free(bm.bitmap);
        return "";
    }
    
    std::cout << "[OcrEngine] Tokenization successful, chunks: " 
              << mtmd_input_chunks_size(chunks) << "\n";

    // Оценка промпта
    llama_pos n_past = 0;
    int32_t eval_status = mtmd_helper_eval_chunks(mtmd_, ctx_, chunks, 0, 0, 1024, true, &n_past);
    if (eval_status != 0) {
        std::cerr << "[OcrEngine] mtmd_helper_eval_chunks failed with code: " << eval_status << "\n";
        mtmd_input_chunks_free(chunks);
        mtmd_bitmap_free(bm.bitmap);
        return "";
    }
    
    std::cout << "[OcrEngine] Evaluation successful, n_past: " << n_past << "\n";

    // Генерация текста
    const llama_vocab* vocab = llama_model_get_vocab(model_);
    llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    // Для OCR используем greedy (детерминированный) декодинг
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
    
    std::cout << "[OcrEngine] Starting generation, max_tokens: " << max_tokens << "\n";
    
    for (int i = 0; i < max_tokens; ++i) {
        llama_token t = llama_sampler_sample(smpl, ctx_, -1);
        
        if (llama_vocab_is_eog(vocab, t)) {
            std::cout << "[OcrEngine] End of generation reached\n";
            break;
        }
        
        char piece[256];
        int n = llama_token_to_piece(vocab, t, piece, sizeof(piece), 0, true);
        if (n > 0) {
            result.append(piece, n);
        }
        
        llama_batch b = llama_batch_get_one(&t, 1);
        if (llama_decode(ctx_, b) != 0) {
            std::cerr << "[OcrEngine] llama_decode failed at token " << i << "\n";
            break;
        }
    }
    
    llama_sampler_free(smpl);
    mtmd_input_chunks_free(chunks);
    mtmd_bitmap_free(bm.bitmap);

    std::cout << "[OcrEngine] Generated " << result.length() << " characters\n";
    std::cout << "[OcrEngine] Raw result: " << result.substr(0, 200) << "...\n";

    // Постобработка
    result = text::stripThink(result);
    std::regex img_re(R"(<img[^>]*>)", std::regex::icase);
    result = std::regex_replace(result, img_re, "");
    
    // Убираем лишние переносы в начале/конце
    size_t start = result.find_first_not_of(" \t\n\r");
    if (start != std::string::npos) {
        result = result.substr(start);
    }
    size_t end = result.find_last_not_of(" \t\n\r");
    if (end != std::string::npos) {
        result = result.substr(0, end + 1);
    }
    
    // Обрезаем до 300 символов для UI
    if (result.length() > 300) {
        // Пытаемся обрезать на границе слова
        size_t cut_pos = result.rfind(' ', 297);
        if (cut_pos == std::string::npos || cut_pos < 200) {
            cut_pos = 297;
        }
        result = result.substr(0, cut_pos) + "...";
    }
    
    return result;
}

} // namespace mary