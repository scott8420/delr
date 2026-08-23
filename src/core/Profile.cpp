// core/Profile -- table four. See the header for what a profile is and why it
// is a search key rather than an identity document.
#include "core/Profile.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace delr::core {
namespace {

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Trim, and collapse every run of whitespace to one space. A term pasted out of
// a web page arrives with a newline and two tabs in the middle of it, and the
// user cannot see that -- so two entries that look identical would not compare
// equal, and the dedup below would keep both.
std::string tidy(const std::string& raw) {
    std::string out;
    bool pending = false;
    for (unsigned char c : raw) {
        if (std::isspace(c)) { pending = !out.empty(); continue; }
        if (pending) { out.push_back(' '); pending = false; }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

// Case-insensitive membership. Terms are compared the way a human reads them:
// "Nashville, TN" and "nashville, tn" are one place, not two.
bool has_term(const std::vector<std::string>& v, const std::string& t) {
    const std::string k = lower(t);
    for (const std::string& e : v)
        if (lower(e) == k) return true;
    return false;
}

void push_unique(std::vector<std::string>& v, const std::string& t) {
    if (!t.empty() && !has_term(v, t)) v.push_back(t);
}

std::vector<std::string> read_terms(const json& j, const char* key) {
    std::vector<std::string> out;
    if (!j.contains(key) || !j[key].is_array()) return out;
    for (const auto& e : j[key])
        if (e.is_string()) push_unique(out, tidy(e.get<std::string>()));
    return out;
}

bool email_shaped(const std::string& e) {
    const std::size_t at = e.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= e.size()) return false;
    if (e.find('@', at + 1) != std::string::npos) return false;
    const std::size_t dot = e.find('.', at + 1);
    return dot != std::string::npos && dot + 1 < e.size();
}

// The plural, said once. Six call sites otherwise, and one of them says
// "1 emails".
std::string plural(std::size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Terms
// ─────────────────────────────────────────────────────────────────────────────
bool profile_is_empty(const Profile& p) {
    return p.full_name.empty() && p.also_known_as.empty() && p.emails.empty()
        && p.phones.empty() && p.usernames.empty() && p.places.empty()
        && p.birth_year == 0 && p.contact_email.empty() && p.note.empty();
}

std::vector<std::string> terms_parse(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) push_unique(out, tidy(line));
    return out;
}

std::string terms_join(const std::vector<std::string>& terms) {
    std::string out;
    for (const std::string& t : terms) {
        if (!out.empty()) out.push_back('\n');
        out += t;
    }
    return out;
}

std::string phone_digits(const std::string& raw) {
    std::string d;
    for (unsigned char c : raw)
        if (std::isdigit(c)) d.push_back(static_cast<char>(c));
    // A leading country code on a US number, dropped so the ten digits below
    // are the ten digits a page prints. Anything longer is not something this
    // function should be guessing about.
    if (d.size() == 11 && d.front() == '1') d.erase(0, 1);
    return d.size() >= 7 ? d : std::string();
}

std::vector<std::string> phone_variants(const std::string& raw) {
    const std::string d = phone_digits(raw);
    if (d.empty()) return {};
    if (d.size() != 10) {
        // Not a shape we know how to print. Return it as typed, once: a needle
        // we cannot format is better than a needle we format wrongly.
        const std::string t = tidy(raw);
        return t.empty() ? std::vector<std::string>{} : std::vector<std::string>{t};
    }
    const std::string a = d.substr(0, 3), b = d.substr(3, 3), c = d.substr(6, 4);
    return {
        d,                              // 6155550100
        a + "-" + b + "-" + c,          // 615-555-0100
        a + "." + b + "." + c,          // 615.555.0100
        a + " " + b + " " + c,          // 615 555 0100
        "(" + a + ") " + b + "-" + c,   // (615) 555-0100
        "(" + a + ")" + b + "-" + c,    // (615)555-0100
        "+1 " + a + " " + b + " " + c,  // +1 615 555 0100
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// Needles
// ─────────────────────────────────────────────────────────────────────────────
bool needle_usable(const std::string& term) {
    const std::string t = tidy(term);
    if (t.size() < 2) return false;

    bool all_digits = true;
    std::size_t digits = 0;
    for (unsigned char c : t) {
        if (std::isdigit(c)) { ++digits; continue; }
        if (!std::isspace(c)) all_digits = false;
    }
    // A year is four digits and appears in a copyright line on every page on
    // the web; a house number is three and matches a price. Five is a zip,
    // which is the shortest number that is actually about a person.
    if (all_digits && digits < 5) return false;
    return true;
}

PageNeedles needles_for(const Profile& p) {
    PageNeedles n;
    auto add = [&](const std::string& t) {
        if (needle_usable(t) && !has_term(n.terms, t)) n.terms.push_back(t);
    };

    add(p.full_name);
    for (const std::string& t : p.also_known_as) add(t);
    for (const std::string& t : p.usernames)     add(t);
    for (const std::string& t : p.places)        add(t);
    // Phones become the FORMS a page prints, not the digits as typed.
    for (const std::string& t : p.phones)
        for (const std::string& v : phone_variants(t)) add(v);

    // ANY, not ALL. A listing that prints your name and omits your city is
    // still your listing, and `require_all` here would turn every partial
    // record into `NeedleAbsent` -- which is `NotFound`, which is the app
    // telling you a live listing is gone. Wrong in the direction that matters.
    n.require_all = false;
    return n;
}

std::size_t needle_count(const Profile& p) { return needles_for(p).terms.size(); }

// ─────────────────────────────────────────────────────────────────────────────
// Validation
// ─────────────────────────────────────────────────────────────────────────────
std::vector<std::string> profile_validate(const Profile& p) {
    std::vector<std::string> errs;

    for (const std::string& e : p.emails)
        if (!email_shaped(e))
            errs.push_back("\"" + e + "\" does not look like an email address.");

    if (!p.contact_email.empty()) {
        if (!email_shaped(p.contact_email))
            errs.push_back("The contact address does not look like an email address.");
        else if (!has_term(p.emails, p.contact_email))
            // The invariant that keeps s15 honest: an opt-out cannot be filed
            // from an address the user never listed here.
            errs.push_back("The contact address is not one of the addresses "
                           "above. Add it to Email addresses first.");
    }

    if (p.birth_year != 0 && (p.birth_year < 1900 || p.birth_year > 2100))
        errs.push_back("Birth year should be a four-digit year, or blank.");

    for (const std::string& ph : p.phones)
        if (phone_digits(ph).empty())
            errs.push_back("\"" + ph + "\" does not have enough digits to be a "
                                       "phone number.");

    // Duplicates within a field. `terms_parse` drops them on the way in, so
    // reaching here means the file was hand-edited -- which is a supported
    // thing to do and therefore a thing to be told about.
    struct { const char* what; const std::vector<std::string>* v; } fields[] = {
        {"Also known as",  &p.also_known_as},
        {"Email addresses", &p.emails},
        {"Phone numbers",  &p.phones},
        {"Usernames",      &p.usernames},
        {"Places",         &p.places},
    };
    for (const auto& f : fields) {
        std::vector<std::string> seen;
        for (const std::string& t : *f.v) {
            if (has_term(seen, t))
                errs.push_back(std::string(f.what) + " lists \"" + t + "\" twice.");
            else
                seen.push_back(t);
        }
    }
    return errs;
}

std::string profile_summary(const Profile& p) {
    if (profile_is_empty(p))
        return "No profile yet. Until there is one, delr can tell whether a "
               "broker's page loaded -- but not whether the record on it is "
               "yours.";

    const std::size_t names = (p.full_name.empty() ? 0u : 1u) + p.also_known_as.size();
    std::vector<std::string> parts;
    if (names)                 parts.push_back(plural(names, "name", "names"));
    if (!p.emails.empty())     parts.push_back(plural(p.emails.size(), "email", "emails"));
    if (!p.phones.empty())     parts.push_back(plural(p.phones.size(), "phone", "phones"));
    if (!p.usernames.empty())  parts.push_back(plural(p.usernames.size(), "username", "usernames"));
    if (!p.places.empty())     parts.push_back(plural(p.places.size(), "place", "places"));

    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += (i + 1 == parts.size() ? " and " : ", ");
        out += parts[i];
    }
    if (out.empty()) out = "nothing";

    const std::size_t n = needle_count(p);
    out += " — ";
    // The honest sentence. Not "you entered 9 things": the number that matters
    // is how many of them can confirm a page is about you, and it is smaller.
    if (n == 0)
        out += "but nothing here can confirm a page is about you. Add a name, "
               "a place you have lived, or a username.";
    else
        out += plural(n, "search term", "search terms")
             + (n == 1 ? " can confirm a page is about you."
                       : " can confirm a page is about you.");
    return out;
}

std::string profile_log_ref(const Profile& p) {
    return "profile: " + plural(p.full_name.empty() ? 0 : 1, "name", "names")
         + ", " + std::to_string(p.also_known_as.size()) + " aka"
         + ", " + std::to_string(p.emails.size())    + " email"
         + ", " + std::to_string(p.phones.size())    + " phone"
         + ", " + std::to_string(p.usernames.size()) + " user"
         + ", " + std::to_string(p.places.size())    + " place"
         + ", " + std::to_string(needle_count(p))    + " needles";
}

// ─────────────────────────────────────────────────────────────────────────────
// Persistence
// ─────────────────────────────────────────────────────────────────────────────
Profile profile_load(const std::string& file, std::string* error) {
    if (error) error->clear();
    std::ifstream in(file);
    if (!in) return {};  // first run: no file is not an error

    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        if (error) *error = std::string("profile: ") + e.what();
        return {};
    }
    if (!j.is_object() || !j.contains("profile") || !j["profile"].is_object()) {
        if (error) *error = "profile: no \"profile\" object in the file";
        return {};
    }
    const json& o = j["profile"];

    Profile p;
    if (o.contains("full_name") && o["full_name"].is_string())
        p.full_name = tidy(o["full_name"].get<std::string>());
    p.also_known_as = read_terms(o, "also_known_as");
    p.emails        = read_terms(o, "emails");
    p.phones        = read_terms(o, "phones");
    p.usernames     = read_terms(o, "usernames");
    p.places        = read_terms(o, "places");
    if (o.contains("birth_year") && o["birth_year"].is_number_integer())
        p.birth_year = o["birth_year"].get<int>();
    if (o.contains("contact_email") && o["contact_email"].is_string())
        p.contact_email = tidy(o["contact_email"].get<std::string>());
    if (o.contains("note") && o["note"].is_string())
        p.note = o["note"].get<std::string>();
    return p;
}

bool profile_save(const std::string& file, const Profile& p) {
    json o;
    o["full_name"]     = p.full_name;
    o["also_known_as"] = p.also_known_as;
    o["emails"]        = p.emails;
    o["phones"]        = p.phones;
    o["usernames"]     = p.usernames;
    o["places"]        = p.places;
    o["birth_year"]    = p.birth_year;
    o["contact_email"] = p.contact_email;
    o["note"]          = p.note;

    json j;
    j["version"] = 1;
    j["profile"] = std::move(o);

    // 0600 before the first byte, same as the egress policy and for a stronger
    // reason: that file holds one address, this one holds the person.
    {
        std::ofstream create(file, std::ios::app);
        if (!create) return false;
    }
    std::error_code ec;
    std::filesystem::permissions(
        file,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);

    std::ofstream out(file, std::ios::trunc);
    if (!out) return false;
    out << j.dump(2) << "\n";
    return out.good();
}

}  // namespace delr::core
