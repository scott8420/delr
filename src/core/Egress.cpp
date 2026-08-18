#include "core/Egress.hpp"

#include <cctype>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

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
        case DnsMode::Unset:   return "unset";
        case DnsMode::System:  return "system";
        case DnsMode::Pinned:  return "pinned";
        case DnsMode::Proxied: return "proxied";
    }
    return "unset";
}

DnsMode dns_mode_from(const std::string& s) {
    const std::string v = lower(trim(s));
    if (v == "system")  return DnsMode::System;
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

std::vector<std::string> egress_policy_validate(const EgressPolicy& p) {
    std::vector<std::string> out;

    if (trim(p.interface_name).empty())
        out.push_back("egress: no interface named");

    switch (p.dns) {
        case DnsMode::Unset:
            out.push_back("egress: dns mode is unset");
            break;
        case DnsMode::System:
            out.push_back("egress: dns mode 'system' leaks every hostname checked "
                          "to the local network; not permitted");
            break;
        case DnsMode::Pinned:
            if (addr_canonical(p.resolver).empty())
                out.push_back("egress: dns mode 'pinned' with no usable resolver address");
            break;
        case DnsMode::Proxied:
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

    if (p.preflight_ttl_s <= 0)
        out.push_back("egress: preflight lifetime must be positive");

    return out;
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

}  // namespace delr::core
