#include "core/Broker.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <set>

using json = nlohmann::json;

namespace delr::core {

const char* method_name(Method m) {
    switch (m) {
        case Method::Web:     return "web";
        case Method::Email:   return "email";
        case Method::Postal:  return "postal";
        case Method::Phone:   return "phone";
        case Method::Drop:    return "drop";
        case Method::Unknown: return "unknown";
    }
    return "unknown";
}

Method method_from(const std::string& s) {
    if (s == "web")    return Method::Web;
    if (s == "email")  return Method::Email;
    if (s == "postal") return Method::Postal;
    if (s == "phone")  return Method::Phone;
    if (s == "drop")   return Method::Drop;
    return Method::Unknown;
}

const Broker* roster_find(const Roster& r, const std::string& id) {
    for (const auto& b : r) if (b.id == id) return &b;
    return nullptr;
}

std::vector<std::string> roster_validate(const Roster& r) {
    std::vector<std::string> problems;
    std::set<std::string> seen;

    for (std::size_t i = 0; i < r.size(); ++i) {
        const auto& b = r[i];
        const std::string where = "entry " + std::to_string(i) +
                                  (b.id.empty() ? "" : " (" + b.id + ")");

        if (b.id.empty())   problems.push_back(where + ": empty id");
        else if (!seen.insert(b.id).second)
            problems.push_back(where + ": duplicate id");

        if (b.name.empty()) problems.push_back(where + ": empty name");

        if (b.method == Method::Unknown)
            problems.push_back(where + ": unknown opt-out method");
        if (b.method == Method::Web && b.opt_out_url.empty())
            problems.push_back(where + ": method 'web' with no opt_out_url");
        if (b.method == Method::Email && b.opt_out_email.empty())
            problems.push_back(where + ": method 'email' with no opt_out_email");

        if (b.recheck_days <= 0)
            problems.push_back(where + ": recheck_days must be positive");
    }
    return problems;
}

Roster roster_load(const std::string& file, std::string* error) {
    Roster out;
    std::ifstream in(file);
    if (!in) return out;   // first-run tolerant: absent is not an error

    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return out;
    }
    if (!j.contains("brokers") || !j["brokers"].is_array()) {
        if (error) *error = "no 'brokers' array";
        return out;
    }

    for (const auto& e : j["brokers"]) {
        Broker b;
        b.id            = e.value("id", "");
        b.name          = e.value("name", "");
        b.site          = e.value("site", "");
        b.method        = method_from(e.value("method", "unknown"));
        b.opt_out_url   = e.value("opt_out_url", "");
        b.opt_out_email = e.value("opt_out_email", "");
        b.requires_id   = e.value("requires_id", false);
        b.recheck_days  = e.value("recheck_days", 45);
        b.ca_registered = e.value("ca_registered", false);
        b.notes         = e.value("notes", "");
        out.push_back(std::move(b));
    }
    return out;
}

bool roster_save(const std::string& file, const Roster& r) {
    json arr = json::array();
    for (const auto& b : r) {
        arr.push_back({
            {"id",            b.id},
            {"name",          b.name},
            {"site",          b.site},
            {"method",        method_name(b.method)},
            {"opt_out_url",   b.opt_out_url},
            {"opt_out_email", b.opt_out_email},
            {"requires_id",   b.requires_id},
            {"recheck_days",  b.recheck_days},
            {"ca_registered", b.ca_registered},
            {"notes",         b.notes},
        });
    }
    json j;
    j["version"] = 1;
    j["brokers"] = std::move(arr);

    std::ofstream out(file);
    if (!out) return false;
    out << j.dump(2) << "\n";
    return out.good();
}

}  // namespace delr::core
