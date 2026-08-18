#include "net/Observer.hpp"
#include "net/Fetch.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

namespace delr::net {
namespace {

// A socket that closes itself. Four early returns below and none of them can
// leak a descriptor, which matters more here than usual: this runs on every
// check for the life of the app.
class Fd {
public:
    explicit Fd(int f) : fd_(f) {}
    ~Fd() { if (fd_ >= 0) ::close(fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
    bool ok() const { return fd_ >= 0; }
private:
    int fd_;
};

// sockaddr -> printable address. inet_ntop rather than getnameinfo, on purpose:
// getnameinfo appends a scope id to link-locals ("fe80::1%wg0") and would
// resolve names if asked. `core::iface_pick_address` never picks a link-local
// anyway, but a reading that carries a suffix on one platform and not another
// is a reading two machines disagree about.
std::string addr_of(const struct sockaddr* sa) {
    if (!sa) return {};
    char buf[INET6_ADDRSTRLEN] = {0};
    if (sa->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const struct sockaddr_in*>(sa);
        if (!inet_ntop(AF_INET, &in->sin_addr, buf, sizeof buf)) return {};
        return buf;
    }
    if (sa->sa_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const struct sockaddr_in6*>(sa);
        if (!inet_ntop(AF_INET6, &in6->sin6_addr, buf, sizeof buf)) return {};
        return buf;
    }
    return {};   // AF_PACKET and friends: not an address we have questions about
}

}  // namespace

// ── Reading one: the interface ───────────────────────────────────────────────
IfaceReading iface_read(const std::string& interface_name) {
    IfaceReading r;
    if (interface_name.empty()) return r;   // unconfigured reads as absent

    struct ifaddrs* head = nullptr;
    if (::getifaddrs(&head) != 0 || !head) return r;

    for (struct ifaddrs* p = head; p; p = p->ifa_next) {
        if (!p->ifa_name || interface_name != p->ifa_name) continue;

        // Present as soon as the name appears, whatever family this entry is
        // for: an interface with only an AF_PACKET entry exists and has no
        // address, and "present but unusable" is a different verdict from
        // "not there" -- NoInterface tells the user to start the VPN, and that
        // would be the wrong instruction.
        r.present = true;
        if ((p->ifa_flags & IFF_UP) && (p->ifa_flags & IFF_RUNNING)) r.up = true;

        const std::string a = addr_of(p->ifa_addr);
        if (!a.empty()) r.addresses.push_back(a);
    }
    ::freeifaddrs(head);
    return r;
}

std::vector<std::string> iface_list() {
    std::vector<std::string> out;
    struct ifaddrs* head = nullptr;
    if (::getifaddrs(&head) != 0 || !head) return out;
    for (struct ifaddrs* p = head; p; p = p->ifa_next) {
        if (!p->ifa_name) continue;
        const std::string n = p->ifa_name;
        // getifaddrs lists an interface once per address family; the window
        // wants it once.
        if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
    ::freeifaddrs(head);
    std::sort(out.begin(), out.end());
    return out;
}

// ── Reading two: the bind ────────────────────────────────────────────────────
std::string bind_probe(const std::string& address) {
    if (address.empty()) return {};

    // Which family from the address itself, because the socket has to match
    // what we are about to bind to it.
    struct in_addr  v4 {};
    struct in6_addr v6 {};
    const bool is_v4 = inet_pton(AF_INET,  address.c_str(), &v4) == 1;
    const bool is_v6 = !is_v4 && inet_pton(AF_INET6, address.c_str(), &v6) == 1;
    if (!is_v4 && !is_v6) return {};

    // SOCK_DGRAM: no handshake, no packet, nothing leaves. The bind is the
    // whole question -- whether the kernel will give us this address -- and a
    // connected TCP socket would answer a different one at the cost of a round
    // trip on every check.
    Fd fd(::socket(is_v4 ? AF_INET : AF_INET6, SOCK_DGRAM, 0));
    if (!fd.ok()) return {};

    if (is_v4) {
        struct sockaddr_in sa {};
        sa.sin_family = AF_INET;
        sa.sin_port   = 0;          // ephemeral; we are asking about the address
        sa.sin_addr   = v4;
        if (::bind(fd.get(), reinterpret_cast<struct sockaddr*>(&sa), sizeof sa) != 0)
            return {};              // the tunnel is gone. NotBound.
    } else {
        struct sockaddr_in6 sa {};
        sa.sin6_family = AF_INET6;
        sa.sin6_port   = 0;
        sa.sin6_addr   = v6;
        if (::bind(fd.get(), reinterpret_cast<struct sockaddr*>(&sa), sizeof sa) != 0)
            return {};
    }

    // Report what the kernel says we got, not what we asked for. They are the
    // same here by construction, and asking anyway is the difference between a
    // reading and an assumption.
    struct sockaddr_storage got {};
    socklen_t len = sizeof got;
    if (::getsockname(fd.get(), reinterpret_cast<struct sockaddr*>(&got), &len) != 0)
        return {};
    return addr_of(reinterpret_cast<struct sockaddr*>(&got));
}

// ── Asking the kernel which way out it would take ────────────────────────────
core::DeviceFound route_device(bool v6) {
    core::DeviceFound out;

    // SOCK_DGRAM and connect(): a route lookup and a source-address assignment,
    // and not one byte on the wire. See the header for why that matters here
    // and why the destination is a documentation address.
    Fd fd(::socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0));
    if (!fd.ok()) {
        // No socket of this family at all: a kernel with v6 disabled, which is
        // the same fact as having no route out of it.
        out.state = core::Detect::NoRoute;
        return out;
    }

    if (v6) {
        struct sockaddr_in6 to {};
        to.sin6_family = AF_INET6;
        to.sin6_port   = htons(9);   // discard. Never reached; nothing is sent.
        if (::inet_pton(AF_INET6, "2001:db8::1", &to.sin6_addr) != 1) return out;
        if (::connect(fd.get(), reinterpret_cast<struct sockaddr*>(&to), sizeof to) != 0) {
            out.state = core::Detect::NoRoute;   // ENETUNREACH: no v6 way out,
            return out;                          // which is not a failure
        }
    } else {
        struct sockaddr_in to {};
        to.sin_family = AF_INET;
        to.sin_port   = htons(9);
        if (::inet_pton(AF_INET, "192.0.2.1", &to.sin_addr) != 1) return out;
        if (::connect(fd.get(), reinterpret_cast<struct sockaddr*>(&to), sizeof to) != 0) {
            out.state = core::Detect::NoRoute;
            return out;
        }
    }

    struct sockaddr_storage got {};
    socklen_t len = sizeof got;
    if (::getsockname(fd.get(), reinterpret_cast<struct sockaddr*>(&got), &len) != 0)
        return out;                              // NotRun: we asked and got no
                                                 // answer, which is a wiring
                                                 // fault rather than a network
                                                 // condition
    const std::string src = addr_of(reinterpret_cast<struct sockaddr*>(&got));
    if (src.empty()) return out;

    // Composed from the two readings that already exist rather than a third
    // walk of `getifaddrs`, so there is one definition of "what addresses does
    // this device have" in the file and not two that can drift.
    std::vector<core::DeviceAddress> claims;
    for (const auto& name : iface_list()) {
        const IfaceReading r = iface_read(name);
        for (const auto& a : r.addresses) claims.push_back({name, a});
    }
    return core::device_for_address(src, claims);
}

// ── Reading three: the v6 routing table ──────────────────────────────────────
core::ProbeReadings::RouteSource routes_read(std::string* text_out,
                                             const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        // No such file is the ordinary case on a kernel booted with ipv6.disable
        // =1, and that machine cannot leak over v6 because it has no v6. Any
        // other failure -- permissions, a read error -- is a table we could not
        // read, and `Unread` fails closed.
        return (errno == ENOENT) ? core::ProbeReadings::RouteSource::Absent
                                 : core::ProbeReadings::RouteSource::Unread;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) return core::ProbeReadings::RouteSource::Unread;
    if (text_out) *text_out = ss.str();
    return core::ProbeReadings::RouteSource::Text;
}

// ── All four, plus the preflight ─────────────────────────────────────────────
ObserverConfig observer_config(const core::EgressPolicy& p) {
    ObserverConfig cfg;
    cfg.echo_url   = p.echo_url;
    cfg.canary_url = p.canary_url;
    return cfg;
}

Observation observe(const core::EgressPolicy& p, const ObserverConfig& cfg,
                    std::int64_t now_s) {
    Observation out;
    core::ProbeReadings& r = out.readings;
    r.observed_at_s = now_s;

    // One and two.
    const IfaceReading iface = iface_read(p.interface_name);
    r.interface_present   = iface.present;
    r.interface_up        = iface.up;
    r.interface_addresses = iface.addresses;

    // The address the CORE picks, not one this file chose. Both sides of the
    // bind comparison must name the same address the same way, and there is
    // exactly one function in the codebase entitled to say which one that is.
    const std::string want = core::iface_pick_address(r.interface_addresses);
    r.bound_address = bind_probe(want);
    r.bound         = !r.bound_address.empty();

    // Three.
    r.routes = routes_read(&r.routes_text);
    if (r.routes == core::ProbeReadings::RouteSource::Unread)
        out.notes.push_back("The IPv6 routing table could not be read, so a "
                            "route around the tunnel cannot be ruled out.");

    // Nothing below this line can happen without a bind, and that is the
    // killswitch holding inside the preflight itself. A dead tunnel does not
    // get a preflight that quietly went out the front door and came back
    // reporting the home address as the exit.
    if (!r.bound) {
        if (iface.present && !iface.up)
            out.notes.push_back("The tunnel interface is present but not "
                                "running, so nothing was sent.");
        else if (!iface.present)
            out.notes.push_back("The tunnel interface is not present, so "
                                "nothing was sent.");
        else
            out.notes.push_back("The tunnel interface is up but its address "
                                "could not be used, so nothing was sent.");
        return out;
    }

    if (!fetch_available()) {
        out.notes.push_back("This build has no network support compiled in, so "
                            "the tunnel could not be checked.");
        return out;
    }

    FetchBinding binding;
    binding.bind_address = r.bound_address;
    if (p.dns == core::DnsMode::Proxied) binding.proxy = p.proxy;
    if (p.dns == core::DnsMode::Pinned) {
        if (fetch_can_pin_dns()) {
            binding.dns_servers = p.resolver;
            // What the shim CONFIGURED, which is the question `egress_check`
            // asks -- not who answered, which is the canary's question. Probe's
            // header is emphatic about the difference and this is the line it
            // was written about.
            r.resolver_used = p.resolver;
        } else {
            // The option was refused by the library. Leaving `resolver_used`
            // empty produces `ResolverMismatch` and no fetch happens, which is
            // right: a pinned resolver that silently didn't pin is the
            // system resolver, and that is a refusal by design.
            out.notes.push_back("This computer's network library cannot send "
                                "lookups to a named resolver, so pinning one "
                                "would not take effect. Route lookups through "
                                "the proxy instead.");
        }
    }

    FetchRequest req;
    req.timeout_s         = cfg.timeout_s;
    req.connect_timeout_s = cfg.timeout_s;
    req.max_bytes         = 256u * 1024u;   // an echo that needs more is wrong

    // Four: the echo. Where did we come from, as far as the far end can see.
    if (!cfg.echo_url.empty()) {
        req.url = cfg.echo_url;
        const FetchResult e = fetch_unchecked(req, binding);
        if (fetch_ok(e) && e.status == 200) {
            r.echo_ran  = true;
            r.echo_body = e.body;
        } else {
            out.notes.push_back("The tunnel's exit could not be checked, so no "
                                "check was allowed to run.");
        }
    }

    // Five: the canary. Who looked the name up. A separate request rather than
    // a header on the one above, because the whole trick is that its NAME is
    // random and therefore uncacheable, and the echo's name is not.
    if (!cfg.canary_url.empty()) {
        req.url = cfg.canary_url;
        const FetchResult c = fetch_unchecked(req, binding);
        if (fetch_ok(c) && c.status == 200) {
            r.canary_ran = true;
            // The body is JSON or HTML and `canary_read` wants an address, so
            // the same reader the echo uses pulls it out: exactly one readable
            // address in the body or nothing. An answer that needs
            // disambiguating is an endpoint pointed at the wrong service, and
            // that is `Failed`, not a guess.
            r.canary_answered_by = core::echo_read(c.body);
            if (r.canary_answered_by.empty())
                out.notes.push_back("The name-lookup check answered with "
                                    "nothing readable, which is not evidence "
                                    "that lookups are private.");
        } else {
            out.notes.push_back("The name-lookup check could not be reached, so "
                                "no check was allowed to run.");
        }
    }

    return out;
}

// ── The baseline: the one deliberately naked request ─────────────────────────
// See the header for why this exists and why it is not a hole. Everything it
// does is a reading already in this file, in a different order and with the
// protections deliberately absent -- there is no new capability here, only the
// one arrangement of the old ones that goes out unprotected, gathered into one
// named function so that arrangement exists in exactly one place and can be
// read in full.
BaselineResult baseline_read(const std::string& interface_name,
                             const ObserverConfig& cfg) {
    BaselineResult out;

    if (!fetch_available()) {
        out.notes.push_back("This build has no network support compiled in, so "
                            "the baseline could not be recorded.");
        return out;
    }
    if (cfg.echo_url.empty() && cfg.canary_url.empty()) {
        out.notes.push_back("There is no address check and no lookup check "
                            "configured, so there is nothing to ask.");
        return out;
    }

    const IfaceReading ifr = iface_read(interface_name);
    if (!ifr.present) {
        out.notes.push_back("That network connection is not on this computer.");
        return out;
    }
    if (!ifr.up) {
        out.notes.push_back("That network connection is not up.");
        return out;
    }

    const std::string addr = core::iface_pick_address(ifr.addresses);
    if (addr.empty()) {
        out.notes.push_back("That network connection has no usable address to "
                            "send from.");
        return out;
    }

    // Bound, like everything else. An unbound handle here would record a
    // baseline that went out by whatever route the kernel felt like, and the
    // whole value of the number is knowing which door it came out of.
    FetchBinding binding;
    binding.bind_address = bind_probe(addr);
    if (binding.bind_address.empty()) {
        out.notes.push_back("This computer would not send from that network "
                            "connection.");
        return out;
    }
    // No proxy, no pinned resolver: see the header. This is the naked path on
    // purpose and the absence of those two lines IS the function.

    FetchRequest req;
    req.timeout_s         = cfg.timeout_s;
    req.connect_timeout_s = cfg.timeout_s;
    req.max_bytes         = 256u * 1024u;

    // ── Half one: what we look like ──────────────────────────────────────────
    if (!cfg.echo_url.empty()) {
        req.url = cfg.echo_url;
        const FetchResult e = fetch_unchecked(req, binding);
        if (!fetch_ok(e) || e.status != 200) {
            out.notes.push_back("The address check could not be reached over "
                                "that connection.");
        } else {
            // The same one-address-or-nothing reader the echo and the canary
            // use. A body with two addresses in it is an endpoint pointed at
            // the wrong service, and a guess here would be recorded as the
            // user's home address and quietly break every future check.
            out.address = core::echo_read(e.body);
            if (out.address.empty())
                out.notes.push_back("The address check answered with nothing "
                                    "readable.");
        }
    } else {
        out.notes.push_back("There is no address-check service configured, so "
                            "this computer's own address was not recorded.");
    }

    // ── Half two: who answers our lookups ────────────────────────────────────
    // Not a second button and not a second moment. This is the same fact about
    // the same tunnel-off moment, and the mode that needs it is refused without
    // it, so gathering it anywhere else is gathering it later or not at all.
    //
    // It runs even when half one failed: the two are independent, and a
    // resolver baseline is worth having whether or not the echo endpoint was
    // up.
    if (!cfg.canary_url.empty()) {
        req.url = cfg.canary_url;
        const FetchResult c = fetch_unchecked(req, binding);
        if (!fetch_ok(c) || c.status != 200) {
            out.notes.push_back("The lookup check could not be reached over "
                                "that connection, so it is not known how "
                                "lookups are answered without the tunnel.");
        } else {
            out.resolver = core::echo_read(c.body);
            if (out.resolver.empty())
                out.notes.push_back("The lookup check answered with nothing "
                                    "readable, so it is not known how lookups "
                                    "are answered without the tunnel.");
        }
    } else {
        out.notes.push_back("There is no lookup-check service configured, so "
                            "how lookups are answered without the tunnel was "
                            "not recorded.");
    }

    return out;
}

}  // namespace delr::net
