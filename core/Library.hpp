#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "Db.hpp"
#include "Guardian.hpp"
#include "ModelRuntime.hpp"

namespace mary {

// Операции пользователя над базой: список, правки, коллекции, профиль, чистка, вопросы.
// Все методы возвращают объект; при ошибке — {"error": "..."}.
class Library {
public:
    Library(Db& db, Guardian& g, ModelRuntime& m) : db_(db), guardian_(g), model_(m) {}

    nlohmann::json list(const nlohmann::json& filter);
    nlohmann::json doc(long long id);
    nlohmann::json updateDoc(const nlohmann::json& body);
    nlohmann::json collections();
    nlohmann::json saveCollection(const nlohmann::json& body);
    nlohmann::json deleteCollection(const nlohmann::json& body);
    nlohmann::json resort();
    nlohmann::json companies();
    nlohmann::json renameCompany(const nlohmann::json& body);
    nlohmann::json profileGet();
    nlohmann::json profileSet(const nlohmann::json& body);
    nlohmann::json cleanup();
    nlohmann::json ask(const std::string& question);

private:
    long long scalar(const std::string& sql, const std::vector<nlohmann::json>& p = {});
    Db& db_;
    Guardian& guardian_;
    ModelRuntime& model_;
};

} // namespace mary