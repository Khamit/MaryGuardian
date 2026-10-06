#pragma once
#include <string>

// Единая точка извлечения текста: PDF, текст (csv/txt/xml/json, utf-8/16/cp1251),
// Office (docx, xlsx, pptx, odt, ods). Для изображений возвращает "" (нужен OCR).
// Формат определяется по содержимому (safety::sniff), а не по расширению.
namespace mary { namespace docread {

// max_chars — максимум символов на выходе; max_pages — только для PDF.
std::string readText(const std::string& path, size_t max_chars, int max_pages = 2);

// Сырые байты файла (для передачи картинки в OCR).
std::string readBinary(const std::string& path);

}} // namespace mary::docread