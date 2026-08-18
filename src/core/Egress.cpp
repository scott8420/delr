#include "core/Egress.hpp"

// For `url_check` only. The endpoints became settings in s9 and something has
// to say whether what the user typed is a url -- and the answer to that
// question already exists, once, in the module that guards a pasted listing.
// Borrowing it beats a second parser here that would drift.
#include "core/Intake.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using json = nlohmann::json;

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

// ── v4 ───────────────────────────────────────────────────────────────────────
// Strict on purpose. "010.1.1.1" is refused rather than read as 8.1.1.1 or
// 10.1.1.1 -- the two readings differ and the ambiguity has been a security bug
// in more parsers than one. An address we cannot read confidently is an address
// we do not have.
bool parse_v4(const std::string& s, unsigned char out[4]) {
    int part = 0;
    std::size_t i = 0;
    while (part < 4) {
        std::size_t start = i;
        int value = 0, digits = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            value = value * 10 + (s[i] - '0');
            ++digits;
            if (digits > 3) return false;
            ++i;
        }
        if (digits == 0 || value > 255) return false;
        if (digits > 1 && s[start] == '0') return false;   // leading zero
        out[part++] = static_cast<unsigned char>(value);
        if (part == 4) break;
        if (i >= s.size() || s[i] != '.') return false;
        ++i;
    }
    return i == s.size();
}

// ── v6 ───────────────────────────────────────────────────────────────────────
// One "::" at most, an embedded v4 tail allowed, eight groups when the dust
// settles. Enough to compare and classify; not a general-purpose parser.
bool parse_v6_side(const std::string& s, std::vector<std::uint16_t>& out, bool allow_v4_tail) {
    if (s.empty()) return true;
    std::size_t i = 0;
    while (true) {
        std::size_t start = i;
        while (i < s.size() && std::isxdigit(static_cast<unsigned char>(s[i]))) ++i;
        const std::size_t len = i - start;

        // An embedded v4 tail ("::ffff:203.0.113.9") occupies the last two groups.
        if (i < s.size() && s[i] == '.') {
            if (!allow_v4_tail) return false;
            unsigned char q[4];
            if (!parse_v4(s.substr(start), q)) return false;
            out.push_back(static_cast<std::uint16_t>((q[0] << 8) | q[1]));
            out.push_back(static_cast<std::uint16_t>((q[2] << 8) | q[3]));
            return true;
        }

        if (len == 0 || len > 4) return false;
        std::uint16_t g = 0;
        for (std::size_t k = start; k < i; ++k) {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[k])));
            const int d = (c >= '0' && c <= '9') ? c - '0' : 10 + (c - 'a');
            g = static_cast<std::uint16_t>(g * 16 + d);
        }
        out.push_back(g);

        if (i == s.size()) return true;
        if (s[i] != ':') return false;
        ++i;
        if (i == s.size()) return false;   // a trailing single ':' is not an address
    }
}

bool parse_v6(const std::string& s, std::uint16_t out[8]) {
    if (s.find(':') == std::string::npos) return false;
    const std::size_t dc = s.find("::");
    if (dc != std::string::npos && s.find("::", dc + 1) != std::string::npos) return false;

    std::vector<std::uint16_t> head, tail;
    if (dc == std::string::npos) {
        if (!parse_v6_side(s, head, true)) return false;
        if (head.size() != 8) return false;
    } else {
        if (!parse_v6_side(s.substr(0, dc), head, false)) return false;
        if (!parse_v6_side(s.substr(dc + 2), tail, true)) return false;
        // "::" has to stand for at least one group; a full eight either side of
        // it means it stands for nothing, which is a different address written
        // wrong rather than this one written short.
        if (head.size() + tail.size() > 7) return false;
    }

    std::size_t n = 0;
    for (auto g : head) out[n++] = g;
    while (n < 8 - tail.size()) out[n++] = 0;
    for (auto g : tail) out[n++] = g;
    return true;
}

bool v6_is_v4_mapped(const std::uint16_t g[8]) {
    return g[0] == 0 && g[1] == 0 && g[2] == 0 && g[3] == 0 && g[4] == 0 && g[5] == 0xffff;
}

AddrKind v4_kind(const unsigned char q[4]) {
    if (q[0] == 0)   return AddrKind::Invalid;      // "this network" -- not a host
    if (q[0] == 127) return AddrKind::Loopback;
    if (q[0] >= 224) return AddrKind::Invalid;      // multicast / reserved: not a unicast host
    if (q[0] == 10)  return AddrKind::Private;
    if (q[0] == 172 && q[1] >= 16 && q[1] <= 31) return AddrKind::Private;
    if (q[0] == 192 && q[1] == 168)              return AddrKind::Private;
    // Carrier-grade NAT. Not a public identity, and an exit "address" inside it
    // means the echo was answered short of the internet.
    if (q[0] == 100 && q[1] >= 64 && q[1] <= 127) return AddrKind::Private;
    if (q[0] == 169 && q[1] == 254)               return AddrKind::LinkLocal;
    return AddrKind::Public;
}

}  // namespace

// ── Addresses ────────────────────────────────────────────────────────────────

const char* addr_kind_name(AddrKind k) {
    switch (k) {
        case AddrKind::Invalid:   return "invalid";
        case AddrKind::Loopback:  return "loopback";
        case AddrKind::Private:   return "private";
        case AddrKind::LinkLocal: return "link-local";
        case AddrKind::Public:    return "public";
    }
    return "invalid";
}

AddrKind addr_kind(const std::string& addr) {
    const std::string s = trim(addr);
    if (s.empty()) return AddrKind::Invalid;

    unsigned char q[4];
    if (parse_v4(s, q)) return v4_kind(q);

    std::uint16_t g[8];
    if (!parse_v6(s, g)) return AddrKind::Invalid;

    if (v6_is_v4_mapped(g)) {
        const unsigned char m[4] = {
            static_cast<unsigned char>(g[6] >> 8), static_cast<unsigned char>(g[6] & 0xff),
            static_cast<unsigned char>(g[7] >> 8), static_cast<unsigned char>(g[7] & 0xff)};
        return v4_kind(m);
    }

    bool all_zero = true;
    for (int i = 0; i < 8; ++i) if (g[i] != 0) all_zero = false;
    if (all_zero) return AddrKind::Invalid;                       // "::"
    if (g[0] == 0 && g[1] == 0 && g[2] == 0 && g[3] == 0 &&
        g[4] == 0 && g[5] == 0 && g[6] == 0 && g[7] == 1) return AddrKind::Loopback;
    if ((g[0] & 0xfe00) == 0xfc00) return AddrKind::Private;       // fc00::/7
    if ((g[0] & 0xffc0) == 0xfe80) return AddrKind::LinkLocal;     // fe80::/10
    if ((g[0] & 0xff00) == 0xff00) return AddrKind::Invalid;       // ff00::/8 multicast
    return AddrKind::Public;
}

std::string addr_canonical(const std::string& addr) {
    const std::string s = trim(addr);
    if (s.empty()) return {};

    char buf[64];
    unsigned char q[4];
    if (parse_v4(s, q)) {
        std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", q[0], q[1], q[2], q[3]);
        return buf;
    }

    std::uint16_t g[8];
    if (!parse_v6(s, g)) return {};

    // A v4-mapped address IS that v4 address, so it canonicalises to it. The
    // observing layer and the config are allowed to disagree about which form
    // they write; they are not allowed to disagree about which address it is.
    if (v6_is_v4_mapped(g)) {
        std::snprintf(buf, sizeof buf, "%u.%u.%u.%u",
                      g[6] >> 8, g[6] & 0xff, g[7] >> 8, g[7] & 0xff);
        return buf;
    }

    std::string out;
    for (int i = 0; i < 8; ++i) {
        std::snprintf(buf, sizeof buf, "%04x", g[i]);
        if (i) out += ':';
        out += buf;
    }
    return lower(out);
}

bool addr_same(const std::string& a, const std::string& b) {
    const std::string ca = addr_canonical(a);
    if (ca.empty()) return false;
    return ca == addr_canonical(b);
}

// ── DNS ──────────────────────────────────────────────────────────────────────

const char* dns_mode_name(DnsMode m) {
    switch (m) {
        case DnsMode::Unset:          return "unset";
        case DnsMode::System:         return "system";
        case DnsMode::SystemVerified: return "system-verified";
        case DnsMode::Pinned:         return "pinned";
        case DnsMode::Proxied:        return "proxied";
    }
    return "unset";
}

DnsMode dns_mode_from(const std::string& s) {
    const std::string v = lower(trim(s));
    if (v == "system")  return DnsMode::System;
    // Spelled out, and NOT reached by any prefix or fuzzy match on "system".
    // The whole point of two values is that a hand-edited `"dns": "system"`
    // keeps meaning the refused thing; a loader generous enough to read it as
    // the permissive one would be the sanitising loader this file's header
    // spends a paragraph refusing to be.
    if (v == "system-verified") return DnsMode::SystemVerified;
    if (v == "pinned")  return DnsMode::Pinned;
    if (v == "proxied") return DnsMode::Proxied;
    return DnsMode::Unset;   // including anything unrecognised: unknown means unset means refused
}

const char* canary_name(Canary c) {
    switch (c) {
        case Canary::NotRun: return "not-run";
        case Canary::Clean:  return "clean";
        case Canary::Failed: return "failed";
        case Canary::Leaked: return "leaked";
    }
    return "not-run";
}

// ── Verdicts ─────────────────────────────────────────────────────────────────

const char* verdict_name(Verdict v) {
    switch (v) {
        case Verdict::Pass:             return "pass";
        case Verdict::Unconfigured:     return "unconfigured";
        case Verdict::DnsUnset:         return "dns-unset";
        case Verdict::DnsSystem:        return "dns-system";
        case Verdict::ResolverMissing:  return "resolver-missing";
        case Verdict::ProxyMissing:     return "proxy-missing";
        case Verdict::ResolverBaselineMissing: return "resolver-baseline-missing";
        case Verdict::CanaryDisabled:   return "canary-disabled";
        case Verdict::ExitUnpinned:     return "exit-unpinned";
        case Verdict::NoInterface:      return "no-interface";
        case Verdict::InterfaceDown:    return "interface-down";
        case Verdict::NotBound:         return "not-bound";
        case Verdict::BindMismatch:     return "bind-mismatch";
        case Verdict::V6OffTunnel:      return "v6-off-tunnel";
        case Verdict::Stale:            return "stale";
        case Verdict::ExitUnobserved:   return "exit-unobserved";
        case Verdict::ExitPrivate:      return "exit-private";
        case Verdict::ExitNaked:        return "exit-naked";
        case Verdict::ExitUnexpected:   return "exit-unexpected";
        case Verdict::CanaryNotRun:     return "canary-not-run";
        case Verdict::CanaryFailed:     return "canary-failed";
        case Verdict::CanaryLeaked:     return "canary-leaked";
        case Verdict::ResolverMismatch: return "resolver-mismatch";
    }
    return "unconfigured";
}

// Every one of these is a sentence a user could act on, and not one of them
// contains an address. "Egress blocked" would be shorter and would tell nobody
// anything.
const char* verdict_text(Verdict v) {
    switch (v) {
        case Verdict::Pass:
            return "Checks go out through the tunnel.";
        case Verdict::Unconfigured:
            return "No tunnel is configured, so nothing can be checked yet. "
                   "Name the network interface your VPN uses.";
        case Verdict::DnsUnset:
            return "No name-lookup method is set. Choose whether lookups go "
                   "through the proxy or through a resolver you name.";
        case Verdict::DnsSystem:
            return "Lookups would use this computer's own resolver, which tells "
                   "your network provider every site being checked even when "
                   "nothing else leaks. Choose a resolver or proxy them.";
        case Verdict::ResolverMissing:
            return "A named resolver was chosen but no resolver address was given.";
        case Verdict::ProxyMissing:
            return "Lookups are set to go through a proxy, but no usable proxy "
                   "address was given. It must be a socks5h address, so that "
                   "names are looked up at the far end rather than here.";
        case Verdict::ResolverBaselineMissing:
            return "Lookups are set to use this computer's own resolver, which "
                   "is only safe if delr can tell when one escapes the tunnel. "
                   "It cannot yet: turn the tunnel OFF once and record how "
                   "lookups are answered without it.";
        case Verdict::CanaryDisabled:
            return "There is no lookup-check service configured, so whether "
                   "name lookups go through the tunnel cannot be tested at all. "
                   "Nothing is allowed out until there is.";
        case Verdict::ExitUnpinned:
            return "Nothing is recorded to compare the tunnel against, so there "
                   "is no way to tell it apart from an ordinary connection. "
                   "Record how the connection looks without the tunnel first.";
        case Verdict::NoInterface:
            return "The configured network interface is not present. The VPN is "
                   "probably not running.";
        case Verdict::InterfaceDown:
            return "The tunnel interface is present but down.";
        case Verdict::NotBound:
            return "The connection was not tied to the tunnel, so it could have "
                   "gone out over the ordinary connection. Refused.";
        case Verdict::BindMismatch:
            return "The connection was tied to an address that is not the "
                   "tunnel's. Refused.";
        case Verdict::V6OffTunnel:
            return "This computer has an IPv6 route that bypasses the tunnel. "
                   "Requests could leave over it while the tunnel looks healthy.";
        case Verdict::Stale:
            return "The last tunnel check is too old to rely on. Checking again.";
        case Verdict::ExitUnobserved:
            return "The tunnel check did not report where the connection comes "
                   "out, so there is nothing to verify.";
        case Verdict::ExitPrivate:
            return "The tunnel check was answered from inside the local network. "
                   "Something is intercepting it.";
        case Verdict::ExitNaked:
            return "Requests are coming out at this computer's own address. The "
                   "tunnel is down. Refused.";
        case Verdict::ExitUnexpected:
            return "The tunnel comes out somewhere unrecognised. If this is your "
                   "VPN's new exit, add it to the accepted list.";
        case Verdict::CanaryNotRun:
            return "Name lookups were never tested, so there is no evidence they "
                   "go through the tunnel.";
        case Verdict::CanaryFailed:
            return "The name-lookup test could not be completed. That is not "
                   "evidence that lookups are safe.";
        case Verdict::CanaryLeaked:
            return "Name lookups are escaping the tunnel. Every site checked "
                   "would be visible to whoever answers them. Refused.";
        case Verdict::ResolverMismatch:
            return "A different resolver answered than the one configured.";
    }
    return "Egress could not be verified.";
}

bool verdict_clear(Verdict v) { return v == Verdict::Pass; }

// ── Policy validation ────────────────────────────────────────────────────────

bool proxy_url_ok(const std::string& s) {
    const std::string u = trim(s);
    const std::string scheme = "socks5h://";
    if (u.size() <= scheme.size()) return false;
    // Case-insensitive on the scheme only; the authority is not ours to fold.
    for (std::size_t i = 0; i < scheme.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(u[i])) != scheme[i]) return false;

    const std::string authority = u.substr(scheme.size());
    if (authority.empty()) return false;
    // No path, query or fragment: a SOCKS proxy has none, and something that
    // brought one along is a URL that was meant for somewhere else.
    if (authority.find_first_of("/?#") != std::string::npos) return false;

    // host[:port]. A bracketed v6 literal keeps its brackets.
    std::string host = authority, port;
    if (!authority.empty() && authority[0] == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        host = authority.substr(0, close + 1);
        const std::string rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest[0] != ':') return false;
            port = rest.substr(1);
        }
        if (addr_kind(authority.substr(1, close - 1)) == AddrKind::Invalid) return false;
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        }
    }
    if (host.empty()) return false;
    for (char c : host)
        if (std::isspace(static_cast<unsigned char>(c))) return false;

    if (!port.empty()) {
        if (port.size() > 5) return false;
        for (char c : port)
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        const int n = std::atoi(port.c_str());
        if (n <= 0 || n > 65535) return false;
    }
    return true;
}

std::vector<std::string> egress_policy_validate(const EgressPolicy& p) {
    std::vector<std::string> out;

    if (trim(p.interface_name).empty())
        out.push_back("egress: no interface named");

    switch (p.dns) {
        case DnsMode::Unset:
            out.push_back("egress: dns mode is unset");
            break;
        case DnsMode::System:
            out.push_back("egress: dns mode 'system' uses this computer's own "
                          "resolver without checking it; not permitted -- "
                          "'system-verified' is the checked form");
            break;
        case DnsMode::SystemVerified:
            // The mode IS its evidence. Without the baseline the canary cannot
            // recognise the leak it exists to catch, and the mode would be
            // 'system' with a better name.
            if (!naked_resolver_known(p))
                out.push_back("egress: dns mode 'system-verified' with no recorded "
                              "no-tunnel resolver -- the lookup check would have "
                              "nothing to recognise a leak against");
            break;
        case DnsMode::Pinned:
            if (addr_canonical(p.resolver).empty())
                out.push_back("egress: dns mode 'pinned' with no usable resolver address");
            break;
        case DnsMode::Proxied:
            if (!proxy_url_ok(p.proxy))
                out.push_back("egress: dns mode 'proxied' needs a socks5h:// proxy "
                              "address (socks5:// without the 'h' resolves names "
                              "here and leaks them)");
            break;
    }

    int usable_exits = 0;
    for (const auto& e : p.accepted_exits) {
        const AddrKind k = addr_kind(e);
        if (k == AddrKind::Invalid) {
            out.push_back("egress: accepted exit is not a readable address");
        } else if (k != AddrKind::Public) {
            // A private accepted exit would pass a preflight that never left
            // the building.
            out.push_back(std::string("egress: accepted exit is ") + addr_kind_name(k) +
                          ", not a public address");
        } else {
            ++usable_exits;
        }
        if (!p.naked_exit.empty() && addr_same(e, p.naked_exit))
            out.push_back("egress: the accepted exit list contains this machine's "
                          "own address");
    }

    const bool naked_ok = !p.naked_exit.empty() && addr_kind(p.naked_exit) == AddrKind::Public;
    if (!p.naked_exit.empty() && !naked_ok)
        out.push_back("egress: the recorded no-tunnel address is not a public address");

    if (usable_exits == 0 && !naked_ok)
        out.push_back("egress: no accepted exit and no no-tunnel baseline -- a "
                      "preflight would have nothing to compare against");

    // Same shape as the exit list above, on the other axis, and reported per
    // entry rather than as a count: a list with one bad address in it needs one
    // line removed, and "some of these are wrong" is not an instruction.
    for (const auto& rz : p.naked_resolvers) {
        if (addr_kind(rz) != AddrKind::Public)
            out.push_back("egress: a recorded no-tunnel resolver is not a public "
                          "address");
    }

    if (p.preflight_ttl_s <= 0)
        out.push_back("egress: preflight lifetime must be positive");

    // The endpoints. `url_check` is the codebase's one answer to "is this a URL
    // we would touch", borrowed rather than re-derived -- a second URL opinion
    // living in this file is exactly the drift the pump comment warns about.
    if (trim(p.echo_url).empty()) {
        out.push_back("egress: no address-check endpoint -- the preflight would "
                      "have no way to see where the tunnel comes out");
    } else if (url_check(p.echo_url) != UrlProblem::None) {
        out.push_back(std::string("egress: the address-check endpoint is not a "
                                  "usable url (") +
                      url_problem_name(url_check(p.echo_url)) + ")");
    }

    if (trim(p.canary_url).empty()) {
        out.push_back("egress: no lookup-check endpoint -- whether name lookups "
                      "leave the tunnel cannot be tested, so nothing is allowed out");
    } else if (url_check(p.canary_url) != UrlProblem::None) {
        out.push_back(std::string("egress: the lookup-check endpoint is not a "
                                  "usable url (") +
                      url_problem_name(url_check(p.canary_url)) + ")");
    }

    return out;
}

bool naked_resolver_known(const EgressPolicy& p) {
    for (const auto& rz : p.naked_resolvers)
        if (addr_kind(rz) == AddrKind::Public) return true;
    return false;
}

// ── The decision ─────────────────────────────────────────────────────────────

Verdict egress_check(const EgressPolicy& p, const EgressObservation& o,
                     std::int64_t now_s) {
    // ── Policy. True whatever was observed. ──────────────────────────────────
    if (trim(p.interface_name).empty()) return Verdict::Unconfigured;
    if (p.dns == DnsMode::Unset)        return Verdict::DnsUnset;
    if (p.dns == DnsMode::System)       return Verdict::DnsSystem;
    if (p.dns == DnsMode::Pinned && addr_canonical(p.resolver).empty())
        return Verdict::ResolverMissing;
    if (p.dns == DnsMode::Proxied && !proxy_url_ok(p.proxy))
        return Verdict::ProxyMissing;
    // The load-bearing line of the whole mode. `SystemVerified` differs from
    // `System` by exactly one thing -- that a leak would be RECOGNISED -- and
    // recognising it requires knowing what the resolver looks like with the
    // tunnel down. No baseline, no recognition, no mode.
    if (p.dns == DnsMode::SystemVerified && !naked_resolver_known(p))
        return Verdict::ResolverBaselineMissing;
    // In every mode, not just that one: the canary has always been a hard gate
    // and an unconfigured endpoint has always refused. What is new is that a
    // person can now empty the box, so the refusal gets to say which box.
    if (trim(p.canary_url).empty()) return Verdict::CanaryDisabled;
    // The echo endpoint gets NO matching verdict here, and the asymmetry is on
    // purpose. An empty echo lands as `ExitUnobserved`, which is a true and
    // actionable sentence about the same fact -- and the echo is not the
    // guarantee for any mode, because whatever it answers is checked against a
    // baseline the user recorded. The canary under `SystemVerified` is the only
    // guarantee there is, which is what earns it a name at the policy layer.
    // The validator names both.

    int usable_exits = 0;
    for (const auto& e : p.accepted_exits)
        if (addr_kind(e) == AddrKind::Public) ++usable_exits;
    const bool naked_known = addr_kind(p.naked_exit) == AddrKind::Public;
    if (usable_exits == 0 && !naked_known) return Verdict::ExitUnpinned;

    // ── Can anything leave off-tunnel? Current state, not evidence. ──────────
    if (!o.interface_present) return Verdict::NoInterface;
    if (!o.interface_up)      return Verdict::InterfaceDown;
    if (!o.bound)             return Verdict::NotBound;
    if (!addr_same(o.bound_address, o.interface_address)) return Verdict::BindMismatch;
    if (o.v6_default_offtunnel) return Verdict::V6OffTunnel;

    // ── Is what we know still current? ───────────────────────────────────────
    // A preflight never run reads as stale rather than as its own state: the
    // instruction is the same one ("look again"), and a separate verdict for it
    // would be a second name for the same fix. A timestamp from the future is
    // stale too -- the clock moved under us, so the age is unknown, and unknown
    // age fails closed like everything else here.
    if (o.observed_at_s <= 0)                            return Verdict::Stale;
    if (now_s < o.observed_at_s)                         return Verdict::Stale;
    if (now_s - o.observed_at_s > p.preflight_ttl_s)     return Verdict::Stale;

    // ── Is this the tunnel I meant? ──────────────────────────────────────────
    const AddrKind exit_kind = addr_kind(o.observed_exit);
    if (exit_kind == AddrKind::Invalid) return Verdict::ExitUnobserved;
    if (exit_kind != AddrKind::Public)  return Verdict::ExitPrivate;

    // Naked is checked BEFORE the accepted list, deliberately: if this address
    // ever reaches the accepted list -- pasted by a user during an outage, when
    // "trust this exit" was the button that made the error go away -- the
    // policy would have quietly accepted the one address it exists to refuse.
    if (naked_known && addr_same(o.observed_exit, p.naked_exit)) return Verdict::ExitNaked;

    if (usable_exits > 0) {
        bool accepted = false;
        for (const auto& e : p.accepted_exits)
            if (addr_same(o.observed_exit, e)) { accepted = true; break; }
        if (!accepted) return Verdict::ExitUnexpected;
    }
    // With no accepted list, "public, and not our own address" is the whole of
    // what can be known, and it is enough to catch the failure that matters:
    // the tunnel dropping and the request going out as us.

    // ── Did the name lookup go with it? ──────────────────────────────────────
    if (o.canary == Canary::NotRun) return Verdict::CanaryNotRun;
    if (o.canary == Canary::Failed) return Verdict::CanaryFailed;
    if (o.canary == Canary::Leaked) return Verdict::CanaryLeaked;

    // Only Pinned can mismatch: under Proxied the resolver is the proxy's
    // business by design, and demanding an address there would be asking the
    // observer to invent one.
    if (p.dns == DnsMode::Pinned && !addr_same(o.observed_resolver, p.resolver))
        return Verdict::ResolverMismatch;

    return Verdict::Pass;
}

// ── Where this meets the caseload ────────────────────────────────────────────

Case apply_egress_refusal(const Case& c, const std::string& today, int retry_days) {
    Case n = apply_check(c, Outcome::Indeterminate, Reason::NoTunnel, today,
                         retry_days < 1 ? 1 : retry_days);
    // apply_check bumped the streak, and it was right to for every other
    // Indeterminate. Not this one: the streak is a claim about the LISTING, and
    // our tunnel being down is a claim about us. Left to accumulate it would
    // read, months later, as a broker that keeps refusing us -- and could argue
    // a case toward Abandoned on the strength of a lapsed VPN subscription.
    n.consecutive_failures = c.consecutive_failures;
    return n;
}

std::string egress_log_ref(const EgressPolicy& p, Verdict v) {
    const std::string iface = trim(p.interface_name);
    return "egress:" + (iface.empty() ? std::string("-") : iface) + "/" + verdict_name(v);
}

// ── The pump ─────────────────────────────────────────────────────────────────

EgressPolicy egress_policy_load(const std::string& file, std::string* error) {
    EgressPolicy p;   // default-constructed: refuses everything
    std::ifstream in(file);
    if (!in) return p;   // first-run tolerant: absent is not an error

    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return p;   // and a half-parsed policy is never returned
    }
    if (!j.contains("egress") || !j["egress"].is_object()) {
        if (error) *error = "no 'egress' object";
        return p;
    }
    const json& e = j["egress"];

    p.interface_name = e.value("interface", "");
    p.naked_exit     = e.value("naked_exit", "");
    // Absent in every policy written before s11. Absent means "we do not know
    // what it was recorded on", which is exactly true of a baseline recorded
    // before anybody wrote this down, and reads as `TunnelCheck::Unchecked`
    // rather than as a wrong answer.
    p.naked_device   = e.value("naked_device", "");
    // Verbatim, including "system" -- see the header. The refusal that names
    // the real problem is worth more than a loader that tidies it away.
    p.dns            = dns_mode_from(e.value("dns", "unset"));
    p.resolver       = e.value("resolver", "");
    p.proxy          = e.value("proxy", "");
    p.preflight_ttl_s = e.value("preflight_ttl_s", static_cast<std::int64_t>(300));

    // Defaulting to the value the STRUCT already holds, not to a literal
    // repeated here. An absent key is a policy written before s9 and keeps the
    // shipped endpoint; a key present and empty is a person who cleared the box
    // and keeps meaning it. Those are different facts and a loader that folded
    // them together would quietly re-enable a check the user turned off.
    p.echo_url        = e.value("echo_url", p.echo_url);
    p.canary_url      = e.value("canary_url", p.canary_url);

    if (e.contains("accepted_exits") && e["accepted_exits"].is_array())
        for (const auto& a : e["accepted_exits"])
            if (a.is_string()) p.accepted_exits.push_back(a.get<std::string>());

    if (e.contains("naked_resolvers") && e["naked_resolvers"].is_array())
        for (const auto& a : e["naked_resolvers"])
            if (a.is_string()) p.naked_resolvers.push_back(a.get<std::string>());

    // Absent in every policy written before this session, which is correct: a
    // one-device tunnel needs no siblings, and an empty list restores exactly
    // the pre-s10 behaviour rather than loosening anything by default.
    if (e.contains("tunnel_devs") && e["tunnel_devs"].is_array())
        for (const auto& a : e["tunnel_devs"])
            if (a.is_string()) p.tunnel_devs.push_back(a.get<std::string>());

    return p;
}

bool egress_policy_save(const std::string& file, const EgressPolicy& p) {
    json exits = json::array();
    for (const auto& a : p.accepted_exits) exits.push_back(a);
    json resolvers = json::array();
    for (const auto& a : p.naked_resolvers) resolvers.push_back(a);
    json devs = json::array();
    for (const auto& a : p.tunnel_devs) devs.push_back(a);

    json e;
    e["interface"]       = p.interface_name;
    e["tunnel_devs"]     = std::move(devs);
    e["accepted_exits"]  = std::move(exits);
    e["naked_exit"]      = p.naked_exit;
    e["naked_device"]    = p.naked_device;
    e["naked_resolvers"] = std::move(resolvers);
    e["dns"]             = dns_mode_name(p.dns);
    e["resolver"]        = p.resolver;
    e["proxy"]           = p.proxy;
    e["preflight_ttl_s"] = p.preflight_ttl_s;
    e["echo_url"]        = p.echo_url;
    e["canary_url"]      = p.canary_url;

    json j;
    j["version"] = 1;
    j["egress"]  = std::move(e);

    // Mode 0600 BEFORE the first byte of content, not after. Creating a
    // world-readable file and then narrowing it leaves a window in which the
    // user's home address is on disk and readable, and this is the one file in
    // the tree where that window matters. Failing to set the mode is not fatal
    // -- some filesystems have no opinion about permissions -- but it is not
    // silently skipped either: an unwritable file still returns false below.
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
