#include "core/Intake.hpp"

#include <algorithm>
#include <cctype>

namespace delr::core {
namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && is_space(s[a])) ++a;
    while (b > a && is_space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The scheme, lower-cased, if the string opens with one. "" when it doesn't --
// which is the common paste, not an error (see url_check).
std::string scheme_of(const std::string& s) {
    std::size_t i = 0;
    if (i >= s.size() || !std::isalpha(static_cast<unsigned char>(s[0]))) return {};
    while (i < s.size()) {
        const char c = s[i];
        if (c == ':') return lower(s.substr(0, i));
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.')
            return {};
        ++i;
    }
    return {};
}

// Everything after the scheme, with its "//" removed.
std::string authority_and_rest(const std::string& s) {
    const auto colon = s.find(':');
    const std::string sch = scheme_of(s);
    if (sch.empty()) return s;
    std::string rest = s.substr(colon + 1);
    if (rest.rfind("//", 0) == 0) rest = rest.substr(2);
    return rest;
}

// Split "host[:port]/path?query#frag" -- the authority, and everything after.
void split_authority(const std::string& s, std::string& authority, std::string& rest) {
    const auto cut = s.find_first_of("/?#");
    if (cut == std::string::npos) { authority = s; rest.clear(); }
    else                          { authority = s.substr(0, cut); rest = s.substr(cut); }
}

bool host_chars_ok(const std::string& h) {
    for (char c : h)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-')) return false;
    return true;
}

// A host we would be willing to fetch. Deliberately strict: this is the string
// a socket eventually connects to, and a permissive parser here is how a
// malformed paste becomes a request somewhere unintended.
bool host_shape_ok(const std::string& h) {
    if (h.empty() || h.size() > 253)          return false;
    if (!host_chars_ok(h))                    return false;
    if (h.find('.') == std::string::npos)     return false;   // no bare "localhost"
    if (h.front() == '.' || h.back() == '.')  return false;
    if (h.front() == '-' || h.back() == '-')  return false;
    if (h.find("..") != std::string::npos)    return false;
    return true;
}

// The authority minus userinfo and port. Reports whether userinfo was present,
// because that is a refusal rather than something to quietly strip.
std::string host_of_authority(const std::string& authority, bool& had_userinfo,
                              bool& bad_port) {
    had_userinfo = authority.find('@') != std::string::npos;
    bad_port = false;
    std::string h = authority;
    if (had_userinfo) h = h.substr(h.find('@') + 1);
    const auto colon = h.find(':');
    if (colon != std::string::npos) {
        const std::string port = h.substr(colon + 1);
        h = h.substr(0, colon);
        if (port.empty()) bad_port = true;
        for (char c : port) if (!std::isdigit(static_cast<unsigned char>(c))) bad_port = true;
    }
    if (!h.empty() && h.back() == '.') h.pop_back();   // the root dot is legal, and noise
    return lower(h);
}

std::string strip_www(const std::string& h) {
    return h.rfind("www.", 0) == 0 ? h.substr(4) : h;
}

// The comparison form: host without www, plus path and query, fragment gone,
// scheme gone, ANY trailing slash gone. Two pastes of the same listing that
// differ only by scheme, by "www.", or by whether the copy took the final
// slash are the same listing, and adding it twice would count one exposure as
// two in every report downstream.
//
// The trailing slash is dropped HERE and not in url_normalize: a server is
// entitled to treat "/p" and "/p/" as different resources, so the stored URL
// keeps what the user pasted, and only the comparison is forgiving.
std::string url_key(const std::string& url) {
    const std::string n = url_normalize(url);
    if (n.empty()) return {};
    const std::string rest = authority_and_rest(n);
    std::string authority, tail;
    split_authority(rest, authority, tail);
    bool userinfo = false, badport = false;
    if (!tail.empty() && tail.back() == '/') tail.pop_back();
    return strip_www(host_of_authority(authority, userinfo, badport)) + tail;
}

}  // namespace

// ── Problem vocabulary ───────────────────────────────────────────────────────

const char* url_problem_name(UrlProblem p) {
    switch (p) {
        case UrlProblem::None:        return "none";
        case UrlProblem::Empty:       return "empty";
        case UrlProblem::Whitespace:  return "whitespace";
        case UrlProblem::BadScheme:   return "bad-scheme";
        case UrlProblem::NoHost:      return "no-host";
        case UrlProblem::BadHost:     return "bad-host";
        case UrlProblem::HasUserinfo: return "has-userinfo";
    }
    return "none";
}

const char* url_problem_text(UrlProblem p) {
    switch (p) {
        case UrlProblem::None:       return "";
        case UrlProblem::Empty:      return "Paste the address of the listing.";
        case UrlProblem::Whitespace: return "That has a space in it -- paste one address, whole.";
        case UrlProblem::BadScheme:  return "Only web addresses (http / https) can be a listing.";
        case UrlProblem::NoHost:     return "No site name in that address.";
        case UrlProblem::BadHost:    return "That site name doesn't look like a hostname.";
        case UrlProblem::HasUserinfo:return "Addresses with a name before the '@' aren't accepted.";
    }
    return "";
}

// ── URL handling ─────────────────────────────────────────────────────────────

UrlProblem url_check(const std::string& raw) {
    const std::string s = trim(raw);
    if (s.empty()) return UrlProblem::Empty;
    for (char c : s) if (is_space(c)) return UrlProblem::Whitespace;

    const std::string sch = scheme_of(s);
    if (!sch.empty() && sch != "http" && sch != "https") return UrlProblem::BadScheme;
    // A recognised scheme must be followed by "//" -- "https:example.com/x" is
    // a typo, and guessing at it means guessing at where a request goes.
    if (!sch.empty() && s.substr(sch.size()).rfind("://", 0) != 0) return UrlProblem::NoHost;

    std::string authority, rest;
    split_authority(authority_and_rest(s), authority, rest);
    if (authority.empty()) return UrlProblem::NoHost;

    bool userinfo = false, badport = false;
    const std::string host = host_of_authority(authority, userinfo, badport);
    if (userinfo)              return UrlProblem::HasUserinfo;
    if (host.empty())          return UrlProblem::NoHost;
    if (badport)               return UrlProblem::BadHost;
    if (!host_shape_ok(host))  return UrlProblem::BadHost;
    return UrlProblem::None;
}

std::string url_normalize(const std::string& raw) {
    if (url_check(raw) != UrlProblem::None) return {};
    const std::string s = trim(raw);

    std::string sch = scheme_of(s);
    if (sch.empty()) sch = "https";   // typed by hand, not downgraded

    std::string authority, rest;
    split_authority(authority_and_rest(s), authority, rest);

    // Host lower-cased (hostnames are case-insensitive); the port kept, since a
    // non-default port is part of where the request goes.
    bool userinfo = false, badport = false;
    std::string host = host_of_authority(authority, userinfo, badport);
    const auto colon = authority.find(':');
    if (colon != std::string::npos) host += authority.substr(colon);

    // Fragment dropped -- it never reaches the server, so it cannot be part of
    // what a fetch sees. Path and query survive byte-for-byte: broker slugs are
    // case-sensitive and their query ids are load-bearing.
    const auto hash = rest.find('#');
    if (hash != std::string::npos) rest = rest.substr(0, hash);
    if (rest == "/") rest.clear();

    return sch + "://" + host + rest;
}

std::string url_host(const std::string& url) {
    if (trim(url).empty()) return {};
    std::string authority, rest;
    split_authority(authority_and_rest(trim(url)), authority, rest);
    bool userinfo = false, badport = false;
    return strip_www(host_of_authority(authority, userinfo, badport));
}

// ── Matching ─────────────────────────────────────────────────────────────────

const Broker* broker_for_url(const Roster& r, const std::string& url) {
    const std::string host = url_host(url);
    if (host.empty()) return nullptr;

    const Broker* best = nullptr;
    std::size_t   best_len = 0;
    bool          best_exact = false;

    const auto consider = [&](const Broker& b, const std::string& candidate) {
        const std::string bh = url_host(candidate);
        if (bh.empty()) return;
        const bool exact = (host == bh);
        // Subdomain match, and only on a label boundary: "notspokeo.com" must
        // not match "spokeo.com".
        const bool sub = host.size() > bh.size() &&
                         host.compare(host.size() - bh.size(), bh.size(), bh) == 0 &&
                         host[host.size() - bh.size() - 1] == '.';
        if (!exact && !sub) return;
        if (best == nullptr || (exact && !best_exact) ||
            (exact == best_exact && bh.size() > best_len)) {
            best = &b; best_len = bh.size(); best_exact = exact;
        }
    };

    for (const auto& b : r) {
        consider(b, b.site);
        consider(b, b.opt_out_url);
    }
    return best;
}

// ── Ids ──────────────────────────────────────────────────────────────────────

std::string next_case_id(const Caseload& c, const std::string& broker_id,
                         const std::string& today) {
    std::string slug;
    for (char ch : broker_id) {
        const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (std::isalnum(static_cast<unsigned char>(l))) slug += l;
        else if (!slug.empty() && slug.back() != '-')    slug += '-';
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    if (slug.empty()) slug = "case";

    std::string day;
    for (char ch : today) if (std::isdigit(static_cast<unsigned char>(ch))) day += ch;
    if (day.empty()) day = "00000000";

    const std::string base = slug + "-" + day;
    if (!caseload_find(c, base)) return base;
    for (int n = 2; n < 10000; ++n) {
        const std::string tryid = base + "-" + std::to_string(n);
        if (!caseload_find(c, tryid)) return tryid;
    }
    return base + "-x";   // unreachable in practice; never returns a taken id
}

// ── Duplicates ───────────────────────────────────────────────────────────────

const Case* caseload_find_by_url(const Caseload& c, const std::string& url) {
    const std::string key = url_key(url);
    if (key.empty()) return nullptr;
    for (const auto& k : c) {
        // Terminal cases are history, not tracking. A URL that appears on a
        // relisted or abandoned case is available to be tracked again.
        if (k.status == Status::Relisted || k.status == Status::Abandoned) continue;
        if (url_key(k.url) == key) return &k;
    }
    return nullptr;
}

// ── The one call ─────────────────────────────────────────────────────────────

IntakeReport intake_inspect(const Roster& r, const Caseload& c,
                            const std::string& raw_url) {
    IntakeReport rep;
    rep.problem = url_check(raw_url);
    if (rep.problem != UrlProblem::None) return rep;

    rep.normalized = url_normalize(raw_url);
    rep.host       = url_host(rep.normalized);
    rep.broker     = broker_for_url(r, rep.normalized);
    rep.existing   = caseload_find_by_url(c, rep.normalized);
    rep.relist     = rep.existing != nullptr && rep.existing->status == Status::Removed;

    // A relist inherits the broker the old case was filed under, even when the
    // roster has since changed shape. The history has to stay comparable.
    if (rep.existing && rep.broker == nullptr)
        rep.broker = roster_find(r, rep.existing->broker_id);
    return rep;
}

Case intake_new_case(const IntakeReport& rep, const Caseload& c,
                     const std::string& today,
                     const std::vector<Field>& exposes, const std::string& note) {
    Case k;
    if (!rep.ready() || rep.relist) return k;   // empty case: caller checked ready()
    k.broker_id  = rep.broker->id;
    k.id         = next_case_id(c, k.broker_id, today);
    k.url        = rep.normalized;
    k.status     = Status::Found;
    k.provenance = Provenance::None;
    k.outcome    = Outcome::Never;              // WE have not looked. See the header.
    k.reason     = Reason::None;
    k.first_seen = today;
    k.next_check = today;                       // never verified, so due now
    k.exposes    = exposes;
    k.note       = note;
    return k;
}

Case intake_relist_case(const IntakeReport& rep, const Caseload& c,
                        const std::string& today) {
    Case k;
    if (!rep.relist || rep.existing == nullptr) return k;
    k = relist_successor(*rep.existing,
                         next_case_id(c, rep.existing->broker_id, today), today);
    // relist_successor carries the old case's next_check forward, which is the
    // right default when a scheduled check discovers the return. Here the
    // discovery is a human paste and that date is stale by definition, so the
    // successor comes due now.
    k.next_check = today;
    return k;
}

bool caseload_commit(Caseload& c, const Case& fresh) {
    if (fresh.id.empty() || fresh.broker_id.empty() || fresh.url.empty()) return false;
    if (caseload_find(c, fresh.id)) return false;

    if (!fresh.supersedes.empty()) {
        // Find it first, mutate nothing until we know we can do both halves.
        Case* old = nullptr;
        for (auto& k : c) if (k.id == fresh.supersedes) { old = &k; break; }
        if (!old) return false;
        old->status = Status::Relisted;
    }
    c.push_back(fresh);
    return true;
}

}  // namespace delr::core
