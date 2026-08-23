#include "core/Statute.hpp"

#include "core/Case.hpp"      // date_valid, date_add_days

#include <nlohmann/json.hpp>

#include <cctype>
#include <fstream>
#include <set>

using json = nlohmann::json;

namespace delr::core {
namespace {

bool is_alpha_upper(char c) { return c >= 'A' && c <= 'Z'; }

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

}  // namespace

// "US-CA": two upper alpha, a hyphen, two-or-three upper alnum. Shape only --
// this deliberately does not carry a list of the fifty states, because a
// validator that knows the states is a validator somebody has to remember to
// update the day a jurisdiction is added, and the row itself is the authority
// on which places this program has a law for.
bool jurisdiction_valid(const std::string& j) {
    if (j.size() < 5 || j.size() > 6) return false;
    if (!is_alpha_upper(j[0]) || !is_alpha_upper(j[1])) return false;
    if (j[2] != '-') return false;
    for (std::size_t i = 3; i < j.size(); ++i)
        if (!is_alpha_upper(j[i]) && !std::isdigit(static_cast<unsigned char>(j[i])))
            return false;
    return true;
}

std::string jurisdiction_normalize(const std::string& raw) {
    std::string s = trim(raw);
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    // A bare "TN" is what a person types and what a US-shaped form collects.
    // Widened here rather than at every call site, and widened to US ONLY --
    // guessing a country from two letters is exactly the ambiguity the key
    // format exists to remove, so the one place it is resolved is the one
    // place a future non-US caller has to change.
    if (s.size() == 2 && is_alpha_upper(s[0]) && is_alpha_upper(s[1]))
        s = "US-" + s;

    return jurisdiction_valid(s) ? s : std::string{};
}

std::string jurisdiction_region(const std::string& j) {
    if (!jurisdiction_valid(j)) return {};
    return j.substr(3);
}

const Statute* statute_for(const Statutes& s, const std::string& jurisdiction) {
    // An empty residency is the first-run state and the commonest one. It is
    // not an error and it must not match a row: no jurisdiction, no law.
    if (jurisdiction.empty()) return nullptr;
    for (const auto& st : s) if (st.jurisdiction == jurisdiction) return &st;
    return nullptr;
}

const Statute* statute_find(const Statutes& s, const std::string& id) {
    if (id.empty()) return nullptr;
    for (const auto& st : s) if (st.id == id) return &st;
    return nullptr;
}

std::string statute_due(const Statute& s, const std::string& filed_iso) {
    if (s.respond_days <= 0) return {};
    return date_add_days(filed_iso, s.respond_days);
}

std::string statute_due_extended(const Statute& s, const std::string& filed_iso) {
    if (s.respond_days <= 0) return {};
    return date_add_days(filed_iso, s.respond_days + (s.extension_days > 0 ? s.extension_days : 0));
}

std::vector<std::string> statutes_validate(const Statutes& s) {
    std::vector<std::string> problems;
    std::set<std::string> ids;
    std::set<std::string> places;

    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto& st = s[i];
        const std::string where = "entry " + std::to_string(i) +
                                  (st.id.empty() ? "" : " (" + st.id + ")");

        if (st.id.empty()) problems.push_back(where + ": empty id");
        else if (!ids.insert(st.id).second)
            problems.push_back(where + ": duplicate id");

        if (st.jurisdiction.empty())
            problems.push_back(where + ": empty jurisdiction");
        else if (!jurisdiction_valid(st.jurisdiction))
            problems.push_back(where + ": malformed jurisdiction '" + st.jurisdiction + "'");
        // The one duplicate that is not a tidiness complaint: two rows for one
        // place makes `statute_for` return whichever came first in the file,
        // and a person's answer to "which law am I invoking" would depend on
        // roster order. Refuse loudly rather than pick.
        else if (!places.insert(st.jurisdiction).second)
            problems.push_back(where + ": second statute for " + st.jurisdiction);

        if (st.name.empty())
            problems.push_back(where + ": empty name -- a row that cannot be named cannot be cited");

        if (st.respond_days < 0)
            problems.push_back(where + ": negative respond_days");
        if (st.extension_days < 0)
            problems.push_back(where + ": negative extension_days");
    }
    return problems;
}

Statutes statutes_load(const std::string& file, std::string* error) {
    if (error) error->clear();
    Statutes out;

    std::ifstream in(file);
    if (!in) return out;                       // first-run tolerant, on purpose

    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return out;
    }
    if (!j.contains("statutes") || !j["statutes"].is_array()) {
        if (error) *error = "no 'statutes' array";
        return out;
    }

    for (const auto& e : j["statutes"]) {
        Statute st;
        st.id           = e.value("id", "");
        st.jurisdiction = e.value("jurisdiction", "");
        st.name         = e.value("name", "");
        st.short_name   = e.value("short_name", "");
        st.citation     = e.value("citation", "");
        st.url          = e.value("url", "");
        st.respond_days   = e.value("respond_days", 0);
        st.extension_days = e.value("extension_days", 0);
        st.verification_allowed = e.value("verification_allowed", true);
        st.requires_residency_statement =
            e.value("requires_residency_statement", true);
        st.notes = e.value("notes", "");
        out.push_back(std::move(st));
    }
    return out;
}

bool statutes_save(const std::string& file, const Statutes& s) {
    json arr = json::array();
    for (const auto& st : s) {
        arr.push_back({
            {"id",             st.id},
            {"jurisdiction",   st.jurisdiction},
            {"name",           st.name},
            {"short_name",     st.short_name},
            {"citation",       st.citation},
            {"url",            st.url},
            {"respond_days",   st.respond_days},
            {"extension_days", st.extension_days},
            {"verification_allowed",         st.verification_allowed},
            {"requires_residency_statement", st.requires_residency_statement},
            {"notes",          st.notes},
        });
    }
    json j;
    j["version"]  = 1;
    j["statutes"] = std::move(arr);

    std::ofstream out(file);
    if (!out) return false;
    out << j.dump(2) << "\n";
    return out.good();
}

}  // namespace delr::core
