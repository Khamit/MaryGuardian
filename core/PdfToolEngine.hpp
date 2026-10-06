#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include <vector>
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

namespace mary {

class PdfReadEngine {
public:
    PdfReadEngine();
    ~PdfReadEngine();

    bool open(const std::string& path);
    void close();

    nlohmann::json getMetadata() const;
    int getPageCount() const;
    nlohmann::json extractPage(int page_num) const;
    nlohmann::json extractAll() const;
    std::vector<int> search(const std::string& query) const;
    // Первые страницы, не больше max_chars (обрезка по границе UTF-8). Для индексатора.
    std::string extractLeadingText(int max_pages, size_t max_chars) const;
    std::string renderPagePng(int page_num, float zoom = 2.0f) const;
    bool isOpen() const { return doc_ != nullptr; }
    const std::string& getPath() const { return path_; }

private:
    fz_context* ctx_ = nullptr;
    fz_document* doc_ = nullptr;
    std::string path_;
};

class PdfEditEngine {
public:
    PdfEditEngine();
    ~PdfEditEngine();

    bool open(const std::string& path);
    void close();
    bool save(const std::string& output_path);

    // Structural edits
    bool deletePage(int page_num);
    bool rotatePage(int page_num, int degrees);
    bool merge(const std::string& other_path, const std::string& output_path);
    bool split(const std::vector<int>& page_nums, const std::string& output_path);

    // Content edits (require PoDoFo or advanced MuPDF stream editing)
    bool insertText(int page_num, const std::string& text, float x, float y, float font_size = 12.0f);
    bool replaceText(int page_num, const std::string& old_text, const std::string& new_text);
    bool insertImage(int page_num, const std::string& image_path, float x, float y, float w, float h);
    bool removeImage(int page_num, const std::string& image_id);

    bool isOpen() const { return doc_ != nullptr; }
    bool isDirty() const { return dirty_; }

private:
    fz_context* ctx_ = nullptr;
    fz_document* doc_ = nullptr;
    pdf_document* pdf_ = nullptr;
    std::string path_;
    bool dirty_ = false;
};

// Unified facade for both read and edit operations
class PdfToolEngine {
public:
    // Read operations
    nlohmann::json readOpen(const std::string& path);
    nlohmann::json readGetPage(int page);
    nlohmann::json readSearch(const std::string& query);
    void readClose();
    bool isReadOpen() const { return read_engine_.isOpen(); }

    // Edit operations
    nlohmann::json editOpen(const std::string& path);
    nlohmann::json editDeletePage(int page);
    nlohmann::json editRotatePage(int page, int degrees);
    nlohmann::json editMerge(const std::string& other, const std::string& out);
    nlohmann::json editSplit(const std::vector<int>& pages, const std::string& out);
    nlohmann::json editInsertText(int page, const std::string& text, float x, float y);
    nlohmann::json editSave(const std::string& out);
    void editClose();

private:
    PdfReadEngine read_engine_;
    PdfEditEngine edit_engine_;
};

} // namespace mary