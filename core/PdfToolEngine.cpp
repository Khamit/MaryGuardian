#include "core/PdfToolEngine.hpp"
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>   // for pdf_delete_page etc.
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <fstream>

namespace mary {

// ============================================================
// PdfReadEngine
// ============================================================
PdfReadEngine::PdfReadEngine() {
    ctx_ = fz_new_context(
        nullptr,
        nullptr,
        FZ_STORE_UNLIMITED
    );

    if (!ctx_) {
        throw std::runtime_error(
            "Cannot create MuPDF context"
        );
    }

    bool ok = false;

    fz_try(ctx_) {
        fz_register_document_handlers(ctx_);
        ok = true;
    }
    fz_catch(ctx_) {
        std::string error =
            fz_caught_message(ctx_);

        fz_drop_context(ctx_);
        ctx_ = nullptr;

        throw std::runtime_error(
            "Cannot initialize MuPDF: " + error
        );
    }

    (void)ok;
}

PdfReadEngine::~PdfReadEngine() {
    close();

    if (ctx_) {
        fz_drop_context(ctx_);
        ctx_ = nullptr;
    }
}

bool PdfReadEngine::open(const std::string& path) {
    close();

    if (!ctx_) {
        std::cerr << "[PdfReadEngine] Context is null\n";
        return false;
    }

    bool success = false;

    fz_try(ctx_) {
        doc_ = fz_open_document(ctx_, path.c_str());

        if (doc_) {
            path_ = path;
            success = true;
        }
    }
    fz_catch(ctx_) {
        std::cerr
            << "[PdfReadEngine] Cannot open: "
            << path
            << "\n[MuPDF] "
            << fz_caught_message(ctx_)
            << "\n";
    }

    return success;
}

void PdfReadEngine::close() {
    if (doc_) {
        fz_drop_document(ctx_, doc_);
        doc_ = nullptr;
    }

    path_.clear();
}

nlohmann::json PdfReadEngine::getMetadata() const {
    nlohmann::json j;
    if (!doc_) return j;
    j["page_count"] = getPageCount();
    j["filename"] = path_;
    char buf[256];
    fz_try(ctx_) {
        if (fz_lookup_metadata(ctx_, doc_, FZ_META_INFO_TITLE, buf, sizeof(buf)) > 0) j["title"] = buf;
        if (fz_lookup_metadata(ctx_, doc_, FZ_META_INFO_AUTHOR, buf, sizeof(buf)) > 0) j["author"] = buf;
    }
    fz_catch(ctx_) { /* метаданные необязательны */ }
    return j;
}

int PdfReadEngine::getPageCount() const {
    if (!doc_) return 0;
    int n = 0;
    fz_var(n);
    fz_try(ctx_) { n = fz_count_pages(ctx_, doc_); }
    fz_catch(ctx_) { n = 0; }
    return n;
}

nlohmann::json PdfReadEngine::extractPage(int page_num) const {
    nlohmann::json result;
    if (!doc_) { result["error"] = "document_not_open"; return result; }

    int page_count = getPageCount();
    if (page_num < 0 || page_num >= page_count) {
        result["error"] = "page_out_of_range";
        result["page"] = page_num;
        result["page_count"] = page_count;
        return result;
    }

    fz_page* page = nullptr;
    fz_stext_page* text = nullptr;
    std::string full_text;            // вне fz_try: при longjmp деструктор не пропадёт
    fz_var(page);
    fz_var(text);

    fz_try(ctx_) {
        page = fz_load_page(ctx_, doc_, page_num);
        fz_rect bounds = fz_bound_page(ctx_, page);
        result["page"]   = page_num;
        result["width"]  = bounds.x1 - bounds.x0;
        result["height"] = bounds.y1 - bounds.y0;

        fz_stext_options opts{};
        text = fz_new_stext_page_from_page(ctx_, page, &opts);

        for (fz_stext_block* block = text->first_block; block; block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
                for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
                    char utf8[10];
                    int len = fz_runetochar(utf8, ch->c);
                    if (len > 0) full_text.append(utf8, len);
                }
                full_text += '\n';
            }
            full_text += '\n';
        }
    }
    fz_always(ctx_) {
        if (text) fz_drop_stext_page(ctx_, text);
        if (page) fz_drop_page(ctx_, page);
    }
    fz_catch(ctx_) {
        result["error"] = "extraction_failed";
        result["message"] = fz_caught_message(ctx_);
        std::cerr << "[PdfReadEngine] Extraction failed, page " << page_num
                  << ": " << fz_caught_message(ctx_) << '\n';
    }
    if (!result.contains("error")) result["text"] = std::move(full_text);
    return result;
}

std::string PdfReadEngine::extractLeadingText(int max_pages, size_t max_chars) const {
    std::string out;
    int n = std::min(getPageCount(), max_pages);
    for (int i = 0; i < n && out.size() < max_chars; ++i)
        out += extractPage(i).value("text", "");
    if (out.size() > max_chars) {
        out.resize(max_chars);
        while (!out.empty() && (out.back() & 0xC0) == 0x80) out.pop_back();
        if (!out.empty() && (out.back() & 0xC0) == 0xC0) out.pop_back();
    }
    return out;
}

nlohmann::json PdfReadEngine::extractAll() const {
    nlohmann::json j;
    j["pages"] = nlohmann::json::array();
    j["metadata"] = getMetadata();
    int n = getPageCount();
    for (int i = 0; i < n; ++i) {
        j["pages"].push_back(extractPage(i));
    }
    return j;
}

std::vector<int> PdfReadEngine::search(const std::string& query) const {
    std::vector<int> hits;
    int n = getPageCount();
    for (int i = 0; i < n; ++i) {
        auto page = extractPage(i);
        std::string text = page.value("text", "");
        if (text.find(query) != std::string::npos) hits.push_back(i);
    }
    return hits;
}

// ============================================================
// PdfEditEngine  (structural ops via MuPDF)
// ============================================================
PdfEditEngine::PdfEditEngine() {
    ctx_ = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
    if (!ctx_) throw std::runtime_error("Cannot create MuPDF context");

    fz_try(ctx_) {
        fz_register_document_handlers(ctx_);
    }
    fz_catch(ctx_) {
        std::string msg = fz_caught_message(ctx_);
        fz_drop_context(ctx_);
        ctx_ = nullptr;
        throw std::runtime_error("Cannot register MuPDF handlers: " + msg);
    }
}

PdfEditEngine::~PdfEditEngine() {
    close();
    if (ctx_) {
        fz_drop_context(ctx_);
        ctx_ = nullptr;
    }
}

bool PdfEditEngine::open(const std::string& path) {
    close();
    bool success = false;
    fz_try(ctx_) {
        doc_ = fz_open_document(ctx_, path.c_str());
        pdf_ = pdf_specifics(ctx_, doc_);
        if (!pdf_) fz_throw(ctx_, FZ_ERROR_GENERIC, "not a PDF");
        path_ = path;
        dirty_ = false;
        success = true;
    }
    fz_catch(ctx_) {
        std::cerr << "[PdfEditEngine] Cannot open: " << path
                  << "\n[MuPDF] " << fz_caught_message(ctx_) << "\n";
    }
    if (!success) close();
    return success;
}

void PdfEditEngine::close() {
    if (doc_) { fz_drop_document(ctx_, doc_); doc_ = nullptr; }
    pdf_ = nullptr;
    path_.clear();
    dirty_ = false;
}

bool PdfEditEngine::save(const std::string& output_path) {
    if (!pdf_) return false;
    if (output_path == path_) {            // исходник (inbox) никогда не перезаписываем
        std::cerr << "[PdfEditEngine] Refusing to overwrite source file\n";
        return false;
    }
    bool ok = false;
    fz_try(ctx_) {
        pdf_write_options opts = pdf_default_write_options;
        pdf_save_document(ctx_, pdf_, output_path.c_str(), &opts);
        ok = true;
    }
    fz_catch(ctx_) { std::cerr << "[PdfEditEngine] Save failed\n"; ok = false; }
    if (ok) dirty_ = false;
    return ok;
}


bool PdfEditEngine::deletePage(int page_num) {
    if (!pdf_) return false;
    bool ok = false;
    fz_try(ctx_) {
        int n = pdf_count_pages(ctx_, pdf_);
        if (page_num < 0 || page_num >= n) fz_throw(ctx_, FZ_ERROR_GENERIC, "page out of range");
        pdf_delete_page(ctx_, pdf_, page_num);
        dirty_ = true; ok = true;
    }
    fz_catch(ctx_) { ok = false; }
    return ok;
}


bool PdfEditEngine::rotatePage(int page_num, int degrees) {
    if (!pdf_ || degrees % 90 != 0) return false;
    degrees = ((degrees % 360) + 360) % 360;
    bool ok = false;
    fz_try(ctx_) {
        int n = pdf_count_pages(ctx_, pdf_);
        if (page_num < 0 || page_num >= n) fz_throw(ctx_, FZ_ERROR_GENERIC, "page out of range");
        pdf_obj* page_obj = pdf_lookup_page_obj(ctx_, pdf_, page_num);
        int rot = pdf_to_int(ctx_, pdf_dict_get_inheritable(ctx_, page_obj, PDF_NAME(Rotate)));
        pdf_dict_put_int(ctx_, page_obj, PDF_NAME(Rotate), (rot + degrees) % 360);
        dirty_ = true; ok = true;
    }
    fz_catch(ctx_) { ok = false; }
    return ok;
}

std::string PdfReadEngine::renderPagePng(int page_num, float zoom) const {
    if (!doc_) return "";
    std::string out;
    fz_pixmap* pix = nullptr;
    fz_buffer* buf = nullptr;
    fz_var(pix); fz_var(buf);
    fz_try(ctx_) {
        pix = fz_new_pixmap_from_page_number(ctx_, doc_, page_num, fz_scale(zoom, zoom),
                                     fz_device_rgb(ctx_), 0);
        buf = fz_new_buffer_from_pixmap_as_png(ctx_, pix, fz_default_color_params);
        unsigned char* data = nullptr;
        size_t n = fz_buffer_storage(ctx_, buf, &data);
        out.assign((const char*)data, n);
    }
    fz_always(ctx_) {
        if (buf) fz_drop_buffer(ctx_, buf);
        if (pix) fz_drop_pixmap(ctx_, pix);
    }
    fz_catch(ctx_) { out.clear(); }
    return out;
}

bool PdfEditEngine::merge(const std::string& other_path, const std::string& output_path) {
    if (!pdf_) return false;
    fz_document*   other = nullptr;
    pdf_graft_map* map   = nullptr;
    bool ok = false;
    fz_var(other); fz_var(map);
    fz_try(ctx_) {
        other = fz_open_document(ctx_, other_path.c_str());
        pdf_document* other_pdf = pdf_specifics(ctx_, other);
        if (!other_pdf) fz_throw(ctx_, FZ_ERROR_GENERIC, "not a PDF");
        map = pdf_new_graft_map(ctx_, pdf_);
        int count = pdf_count_pages(ctx_, other_pdf);
        for (int i = 0; i < count; ++i)
            pdf_graft_mapped_page(ctx_, map, -1, other_pdf, i);
        ok = true;
    }
    fz_always(ctx_) {
        if (map)   pdf_drop_graft_map(ctx_, map);
        if (other) fz_drop_document(ctx_, other);
    }
    fz_catch(ctx_) { ok = false; }
    if (!ok) return false;
    dirty_ = true;
    return save(output_path);
}

bool PdfEditEngine::split(const std::vector<int>& page_nums, const std::string& output_path) {
    if (!pdf_ || page_nums.empty()) return false;
    pdf_document*  out = nullptr;
    pdf_graft_map* map = nullptr;
    bool ok = false;
    fz_var(out); fz_var(map);
    fz_try(ctx_) {
        out = pdf_create_document(ctx_);
        map = pdf_new_graft_map(ctx_, out);
        for (int p : page_nums)
            pdf_graft_mapped_page(ctx_, map, -1, pdf_, p);
        pdf_write_options opts = pdf_default_write_options;
        pdf_save_document(ctx_, out, output_path.c_str(), &opts);
        ok = true;
    }
    fz_always(ctx_) {
        if (map) pdf_drop_graft_map(ctx_, map);
        if (out) pdf_drop_document(ctx_, out);
    }
    fz_catch(ctx_) { ok = false; }
    return ok;
}

// ---- Content edits: STUBS with note ----
// MuPDF C API for true text replacement requires rewriting page content
// streams (TJ/Tj operators). For production, link PoDoFo or use mutool run.
bool PdfEditEngine::insertText(int page_num, const std::string& text, float x, float y, float font_size) {
    (void)page_num; (void)text; (void)x; (void)y; (void)font_size;
    // TODO: Implement via pdf_new_dict font + pdf_update_stream with BT/ET operators
    // OR use PoDoFo: Podofo::PdfPage::CreateField() / DrawText()
    std::cerr << "[PdfEditEngine] insertText requires PoDoFo or advanced MuPDF stream editing\n";
    return false;
}

bool PdfEditEngine::replaceText(int page_num, const std::string& old_text, const std::string& new_text) {
    (void)page_num; (void)old_text; (void)new_text;
    std::cerr << "[PdfEditEngine] replaceText requires PoDoFo or advanced MuPDF stream editing\n";
    return false;
}

bool PdfEditEngine::insertImage(int page_num, const std::string& image_path, float x, float y, float w, float h) {
    (void)page_num; (void)image_path; (void)x; (void)y; (void)w; (void)h;
    std::cerr << "[PdfEditEngine] insertImage requires PoDoFo or advanced MuPDF stream editing\n";
    return false;
}

bool PdfEditEngine::removeImage(int page_num, const std::string& image_id) {
    (void)page_num; (void)image_id;
    return false;
}

// ============================================================
// PdfToolEngine unified facade
// ============================================================
nlohmann::json PdfToolEngine::readOpen(const std::string& path) {
    if (!read_engine_.open(path)) return {{"error","cannot open PDF"}};
    return {{"status","ok"}, {"metadata", read_engine_.getMetadata()}};
}

nlohmann::json PdfToolEngine::readGetPage(int page) {
    return read_engine_.extractPage(page);
}

nlohmann::json PdfToolEngine::readSearch(const std::string& query) {
    auto hits = read_engine_.search(query);
    return {{"pages", hits}, {"query", query}};
}

void PdfToolEngine::readClose() { read_engine_.close(); }

nlohmann::json PdfToolEngine::editOpen(const std::string& path) {
    if (!edit_engine_.open(path)) return {{"error","cannot open PDF for editing"}};
    return {{"status","ok"}, {"path",path}};
}

nlohmann::json PdfToolEngine::editDeletePage(int page) {
    bool ok = edit_engine_.deletePage(page);
    return {{"ok", ok}, {"action","delete_page"}, {"page",page}};
}

nlohmann::json PdfToolEngine::editRotatePage(int page, int degrees) {
    bool ok = edit_engine_.rotatePage(page, degrees);
    return {{"ok", ok}, {"action","rotate"}, {"page",page}, {"degrees",degrees}};
}

nlohmann::json PdfToolEngine::editMerge(const std::string& other, const std::string& out) {
    bool ok = edit_engine_.merge(other, out);
    return {{"ok", ok}, {"action","merge"}, {"output",out}};
}

nlohmann::json PdfToolEngine::editSplit(const std::vector<int>& pages, const std::string& out) {
    bool ok = edit_engine_.split(pages, out);
    return {{"ok", ok}, {"action","split"}, {"pages",pages}, {"output",out}};
}

nlohmann::json PdfToolEngine::editInsertText(int page, const std::string& text, float x, float y) {
    bool ok = edit_engine_.insertText(page, text, x, y);
    return {{"ok", ok}, {"action","insert_text"}, {"page",page}, {"text",text}, {"x",x}, {"y",y}};
}

nlohmann::json PdfToolEngine::editSave(const std::string& out) {
    bool ok = edit_engine_.save(out);
    return {{"ok", ok}, {"action","save"}, {"path",out}};
}

void PdfToolEngine::editClose() { edit_engine_.close(); }

} // namespace mary