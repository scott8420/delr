#include "core/Probe.hpp"

#include <cctype>
#include <cstddef>
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

// "fe80::1%wg0" -> "fe80::1". getifaddrs/getnameinfo hand back a scope id on
// link-local addresses; it identifies the interface, not the address, and
// nothing downstream compares on it.
std::string strip_zone(const std::string& s) {
    const std::size_t pct = s.find('%');
    return pct == std::string::npos ? s : s.substr(0, pct);
}

std::string clean_addr(const std::string& s) { return strip_zone(trim(s)); }

// Hex field, as `/proc` writes them. Returns false rather than guessing: a
// field we cannot read makes the LINE unreadable, which is a state this file
// reports rather than one it papers over.
bool hex_value(const std::string& s, unsigned long& out) {
    if (s.empty() || s.size() > 16) return false;
    unsigned long v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = v * 16 + static_cast<unsigned long>(d);
    }
    out = v;
    return true;
}

std::vector<std::string> fields_of(const std::string& line) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && is_space(line[i])) ++i;
        const std::size_t start = i;
        while (i < line.size() && !is_space(line[i])) ++i;
        if (i > start) out.push_back(line.substr(start, i - start));
    }
    return out;
}

bool all_zeros(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c != '0') return false;
    return true;
}

bool looks_v6(const std::string& s) { return s.find(':') != std::string::npos; }

// Linux route flags, the three this file cares about.
constexpr unsigned long kRtfUp     = 0x0001;
constexpr unsigned long kRtfReject = 0x0200;

}  // namespace

// ── The v6 routing table ─────────────────────────────────────────────────────

const char* v6_route_name(V6Route r) {
    switch (r) {
        case V6Route::None:       return "none";
        case V6Route::TunnelOnly: return "tunnel-only";
        case V6Route::OffTunnel:  return "off-tunnel";
        case V6Route::Unreadable: return "unreadable";
    }
    return "unreadable";
}

V6Route v6_default_route(const std::string& text, const std::string& interface_name,
                         const std::vector<std::string>& also_tunnel) {
    const std::string want = trim(interface_name);

    // A device counts as the tunnel if it is the bind device or one of the
    // siblings the user named. Exact match on a trimmed name, deliberately --
    // no prefixes, no globbing. `surfshark_*` would let a device named to look
    // like the tunnel be read as the tunnel, and this is the one comparison in
    // the file that decides whether a leak is reported.
    const auto is_tunnel = [&](const std::string& dev) {
        if (!want.empty() && dev == want) return true;
        for (const auto& d : also_tunnel) {
            const std::string t = trim(d);
            if (!t.empty() && dev == t) return true;
        }
        return false;
    };
    bool saw_content = false, read_a_line = false;
    bool off_tunnel = false, on_tunnel = false;

    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos
                                                                          : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        if (trim(line).empty()) continue;
        saw_content = true;

        // dest, dest_len, src, src_len, next_hop, metric, refcnt, use, flags, dev
        const std::vector<std::string> f = fields_of(line);
        if (f.size() < 10) continue;

        unsigned long dest_len = 0, flags = 0;
        if (!hex_value(f[1], dest_len)) continue;
        if (!hex_value(f[8], flags))    continue;
        if (f[0].size() != 32) continue;
        read_a_line = true;

        // A default route and nothing else: /0, to all zeros.
        if (dest_len != 0 || !all_zeros(f[0])) continue;

        if (!(flags & kRtfUp))    continue;   // not installed
        if (flags & kRtfReject)   continue;   // an unreachable default is the
                                              // opposite of a leak
        const std::string dev = f[9];
        if (dev == "lo") continue;

        if (is_tunnel(dev)) on_tunnel = true;
        else                off_tunnel = true;
    }

    if (off_tunnel) return V6Route::OffTunnel;
    if (on_tunnel)  return V6Route::TunnelOnly;
    // Content we could not make ten fields of is a kernel that changed shape
    // under us, or a file that is not the file we think it is. Either way we
    // cannot clear the table, so we do not.
    if (saw_content && !read_a_line) return V6Route::Unreadable;
    return V6Route::None;
}

// ── Which device would a packet actually leave by ────────────────────────────

const char* detect_name(Detect d) {
    switch (d) {
        case Detect::NotRun:    return "not-run";
        case Detect::NoRoute:   return "no-route";
        case Detect::Unmatched: return "unmatched";
        case Detect::Ambiguous: return "ambiguous";
        case Detect::Found:     return "found";
    }
    return "not-run";
}

DeviceFound device_for_address(const std::string& address,
                               const std::vector<DeviceAddress>& devices) {
    DeviceFound out;
    const std::string want = clean_addr(address);
    if (want.empty()) return out;   // NotRun: nobody gave us anything to match

    // Loopback is tracked separately rather than skipped, so that "only lo
    // claims this" comes back as NoRoute -- the honest reading of a route
    // lookup that went nowhere -- instead of Unmatched, which would send a
    // user looking for a missing interface.
    bool loopback_only = false;
    std::string first;
    bool conflict = false;

    for (const auto& d : devices) {
        if (clean_addr(d.address) != want) continue;
        const std::string dev = trim(d.device);
        if (dev.empty()) continue;
        if (dev == "lo") { loopback_only = true; continue; }
        if (first.empty())      first = dev;
        else if (dev != first)  conflict = true;   // the same NAME twice is one
                                                   // device with two entries,
                                                   // which getifaddrs does
                                                   // routinely and is not a
                                                   // conflict
    }

    if (conflict)         { out.state = Detect::Ambiguous; return out; }
    if (!first.empty())   { out.state = Detect::Found; out.device = first; return out; }
    if (loopback_only)    { out.state = Detect::NoRoute; return out; }
    out.state = Detect::Unmatched;
    return out;
}

// ── Is the thing we just detected plausibly a tunnel ─────────────────────────

const char* tunnel_check_name(TunnelCheck t) {
    switch (t) {
        case TunnelCheck::NoDevice:  return "no-device";
        case TunnelCheck::Unchecked: return "unchecked";
        case TunnelCheck::NotUp:     return "not-up";
        case TunnelCheck::Distinct:  return "distinct";
    }
    return "no-device";
}

TunnelCheck tunnel_check(const DeviceFound& detected, const std::string& naked_device) {
    if (detected.state != Detect::Found || trim(detected.device).empty())
        return TunnelCheck::NoDevice;
    const std::string naked = trim(naked_device);
    if (naked.empty()) return TunnelCheck::Unchecked;
    return trim(detected.device) == naked ? TunnelCheck::NotUp : TunnelCheck::Distinct;
}

// ── Which address is "the tunnel's own address" ──────────────────────────────

std::string iface_pick_address(const std::vector<std::string>& addresses) {
    std::string v6;
    for (const auto& raw : addresses) {
        const std::string a = clean_addr(raw);
        const AddrKind k = addr_kind(a);
        if (k == AddrKind::Invalid || k == AddrKind::Loopback || k == AddrKind::LinkLocal)
            continue;
        if (!looks_v6(a)) return a;          // v4 wins outright
        if (v6.empty()) v6 = a;              // first usable v6, held in reserve
    }
    return v6;
}

// ── What the preflight echo said ─────────────────────────────────────────────

std::string echo_read(const std::string& body) {
    // A hostile or merely careless endpoint can answer with a whole web page.
    // We are looking for a short answer; if it is not near the front, this is
    // not an echo service.
    constexpr std::size_t kMax = 4096;
    const std::string s = body.size() > kMax ? body.substr(0, kMax) : body;

    std::string found, found_canonical;
    int distinct = 0;

    std::size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        const bool addr_char = std::isxdigit(static_cast<unsigned char>(c)) ||
                               c == ':' || c == '.';
        if (!addr_char) { ++i; continue; }

        const std::size_t start = i;
        while (i < s.size() && (std::isxdigit(static_cast<unsigned char>(s[i])) ||
                                s[i] == ':' || s[i] == '.')) ++i;

        std::string tok = s.substr(start, i - start);
        // Trailing punctuation only. A leading colon is part of "::1"; a
        // trailing one is the end of a sentence or a JSON key.
        while (!tok.empty() && (tok.back() == '.' || tok.back() == ':')) tok.pop_back();
        if (tok.empty()) continue;

        if (addr_kind(tok) == AddrKind::Invalid) continue;
        const std::string canon = addr_canonical(tok);
        if (canon.empty()) continue;
        if (distinct == 0) { found = tok; found_canonical = canon; ++distinct; }
        else if (canon != found_canonical) { ++distinct; break; }
    }

    return distinct == 1 ? found : std::string{};
}

// ── What the DNS canary said ─────────────────────────────────────────────────

Canary canary_read(const std::string& answered_by, const std::string& naked_exit,
                   const std::vector<std::string>& naked_resolvers) {
    const std::string who = clean_addr(answered_by);
    const AddrKind k = addr_kind(who);
    if (k == AddrKind::Invalid) return Canary::Failed;

    // Our own address asked, or something inside the building did. Both are
    // the lookup going around the tunnel, which is the one thing the canary
    // exists to see.
    if (addr_kind(naked_exit) == AddrKind::Public && addr_same(who, naked_exit))
        return Canary::Leaked;

    // And the one the old comparison could not see: a resolver that answers
    // when the tunnel is DOWN, answering while it is up. This is the ordinary
    // leak -- an ISP recursive, a public address, entirely unremarkable to look
    // at -- and the only thing that makes it recognisable is having watched it
    // with the tunnel off. Every recorded one is checked, because the pool is
    // the point: matching only the most recent sample would be the scalar this
    // was widened out of.
    for (const auto& rz : naked_resolvers) {
        if (addr_kind(rz) == AddrKind::Public && addr_same(who, rz))
            return Canary::Leaked;
    }

    if (k != AddrKind::Public) return Canary::Leaked;

    return Canary::Clean;
}

// ── Readings -> observation ──────────────────────────────────────────────────

EgressObservation observation_from(const ProbeReadings& r, const EgressPolicy& p) {
    EgressObservation o;

    o.interface_present = r.interface_present;
    o.interface_up      = r.interface_up;
    o.interface_address = iface_pick_address(r.interface_addresses);

    o.bound         = r.bound;
    o.bound_address = clean_addr(r.bound_address);

    switch (r.routes) {
        case ProbeReadings::RouteSource::Unread:
            // Nobody looked. Fails closed, like every other unread field here.
            o.v6_default_offtunnel = true;
            break;
        case ProbeReadings::RouteSource::Absent:
            // No such file: this kernel has no v6 stack, so there is no v6
            // path around anything.
            o.v6_default_offtunnel = false;
            break;
        case ProbeReadings::RouteSource::Text: {
            const V6Route v = v6_default_route(r.routes_text, p.interface_name,
                                               p.tunnel_devs);
            // NOTE the flattening: `Unreadable` lands on the same bool as
            // `OffTunnel`, so a table we could not parse is reported to the
            // policy as a leak. That is the right refusal with a slightly wrong
            // sentence attached -- if it ever bites in the field, the fix is a
            // verdict of its own in Egress, not a change here.
            o.v6_default_offtunnel = (v == V6Route::OffTunnel || v == V6Route::Unreadable);
            break;
        }
    }

    o.observed_exit     = r.echo_ran ? echo_read(r.echo_body) : std::string{};
    o.observed_resolver = clean_addr(r.resolver_used);
    o.canary            = r.canary_ran ? canary_read(r.canary_answered_by, p.naked_exit,
                                                     p.naked_resolvers)
                                       : Canary::NotRun;

    o.observed_at_s = r.observed_at_s;
    return o;
}

}  // namespace delr::core
