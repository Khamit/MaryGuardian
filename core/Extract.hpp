#pragma once
#include <string>
#include <vector>

// Чистые функции без состояния: правила извлечения полей из текста и имени файла.
namespace mary { namespace extract {

struct Fields {
    std::string type;                 // invoice|receipt|contract|statement|tax|letter или ""
    std::string date;                 // ISO
    std::string number;
    std::string currency;
    std::string company;
    std::string expires;              // ISO
    std::string expires_basis;        // expires|term|due|net
    long long   amount_minor = -1;
};

std::string parseDate(const std::string& s);               // первая дата в строке → ISO или ""
std::string addDays(const std::string& iso, int days);
Fields      fromText(const std::string& text);
Fields      fromName(const std::string& filename);          // только type и date
std::string normalizeCompany(const std::string& name);
std::string formatMinor(long long minor);                   // 125000 → "1,250.00"

// Единый список типов документов. Один тип = одна встроенная коллекция (Db::init создаёт недостающие).
// Чтобы добавить категорию: допишите строку сюда (id латиницей + имя коллекции для интерфейса) и,
// по желанию, правила в Extract.cpp (fromName / fromText). Подсказка модели, проверки и интерфейс подхватят её сами.
struct DocType { const char* type; const char* collection; };
inline const std::vector<DocType>& docTypes() {
    static const std::vector<DocType> v = {
        {"invoice", "Invoices"}, {"receipt", "Receipts"}, {"credit_note", "Credit notes"},
        {"quote", "Quotes & estimates"}, {"purchase_order", "Purchase orders"}, {"contract", "Contracts"},
        {"statement", "Bank statements"}, {"payslip", "Payroll"}, {"tax", "Tax"}, {"insurance", "Insurance"},
        {"utility", "Utilities"}, {"legal", "Legal & court"}, {"identity", "Identity documents"},
        {"medical", "Medical"}, {"certificate", "Certificates"}, {"report", "Reports"},
        {"letter", "Letters"}, {"other", "Other"}};
    return v;
}

}} // namespace mary::extract