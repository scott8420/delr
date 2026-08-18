#pragma once
#include "core/Egress.hpp"

#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// core/Probe -- the pure half of the observer.
//
// `core/Egress` decides whether a request may leave, as a pure function of a
// policy and an `EgressObservation`. Somebody has to FILL IN that observation,
// and that somebody needs `getifaddrs`, a socket, and `/proc` -- none of which
// belong in a GTK-free, headless-testable core, and none of which can be
// exercised in `delr --selftest`.
//
// But look at what the observer actually does. The syscalls produce BYTES: a
// list of address strings, the contents of a routing table, the body of an HTTP
// response, the answer to a lookup. Everything from those bytes to the
// observation is judgment -- which of an interface's four addresses is "the
// tunnel's own address", whether a routing table line is a default route out
// the wrong hole, whether a response body contains an address we can believe.
// That judgment is the part that can be wrong in interesting ways, and it is
// all pure.
//
// So it lives here, and the syscall shim above becomes the thinnest thing that
// can still be honest: read four things, hand them over verbatim, and say
// plainly which ones it could not read. The same inversion as `Egress` itself
// -- decide it before you can do it -- applied one layer down. When the shim
// lands it cannot invent a new way to look safe, because what the readings MEAN
// was settled here, with no packet in the room.
//
// ── PII WARNING ──────────────────────────────────────────────────────────────
// `ProbeReadings::echo_body` is a network response that contains this machine's
// public address, and `canary_answered_by` is an address too. Same rule as the
// rest of `Egress`: nothing in this file logs, and nothing here is ever
// displayed. The readings go in, an observation comes out, and the only thing
// anything upstream is allowed to say out loud is `egress_log_ref()`.
// ─────────────────────────────────────────────────────────────────────────────
namespace delr::core {

// ── The v6 routing table ─────────────────────────────────────────────────────
// The leak this catches: a v4-only tunnel on a dual-stack host. Every request
// the host prefers to make over v6 -- on a modern desktop, most of them -- goes
// around the tunnel while the tunnel is up and healthy the whole time. Binding
// does not catch it, because the socket binds to a v4 address and the v6
// request was never going to use that socket.
//
// Named states rather than a bool, for the usual reason: "no default route at
// all" and "we could not read the table" are both "not off-tunnel" and mean
// completely different things.
enum class V6Route {
    None,        // no usable v6 default route: v6 goes nowhere, so it leaks nowhere
    TunnelOnly,  // the only default route out is the tunnel. What we want.
    OffTunnel,   // a default route out some other device. The leak.
    Unreadable   // the table was there and we could not read it -- treated as
                 // the leak, because a routing table we cannot parse is one we
                 // cannot clear.
};

const char* v6_route_name(V6Route r);

// Parse `/proc/net/ipv6_route` and say whether v6 can leave without us.
//
// Ten whitespace-separated fields per line; the two that matter are the
// destination prefix length (a default route is /0 to all-zeros) and the device
// at the end. Skipped: routes not marked up, reject routes (an unreachable
// default is the opposite of a leak), and anything on `lo`.
//
// `interface_name` is the bind device; `also_tunnel` are the other devices that
// are ALSO this tunnel. A default route out any of them is TunnelOnly. The
// second argument exists because a tunnel is not always one device -- Surfshark
// splits v4 and v6 across two -- and reading a sibling as a leak refuses a
// correctly-tunnelled machine. See `EgressPolicy::tunnel_devs`.
V6Route v6_default_route(const std::string& proc_net_ipv6_route,
                         const std::string& interface_name,
                         const std::vector<std::string>& also_tunnel = {});

// ── Which device would a packet actually leave by ────────────────────────────
// The v6 check above reads a routing TABLE, because its question is "is there
// any default route out of the tunnel" and only the whole table can answer it.
// Detection asks a different and narrower question -- "if this computer sent a
// packet to the internet right now, which device would carry it" -- and for
// that the table is the wrong instrument.
//
// It is the wrong instrument because a VPN does not have to touch the main
// table to capture traffic, and the two common ones do not. wg-quick installs
// its default into table 51820 and adds an `ip rule` for everything not carrying
// its own fwmark; OpenVPN leaves the main default alone and adds `0.0.0.0/1`
// and `128.0.0.0/1` over the top. In both cases `/proc/net/route` still shows
// the physical connection as the default while every packet goes down the
// tunnel. A detector reading that file would tell a correctly-tunnelled user
// their tunnel was off -- the same class of mistake `V6OffTunnel` made in s10,
// made by the same means: a parser guessing at what the kernel would decide.
//
// So the kernel decides. The shim asks it for a route (see `net::route_device`)
// and gets back an ADDRESS; this file turns that into a DEVICE, which is the
// part with the judgment in it and therefore the part that lives here.
//
// This is a HINT AND NEVER A CHECK. Nothing in the detection path may reach a
// verdict: it fills in a box a person then reads, and `egress_check` goes on
// deciding what it decided before. A detector that was wrong writes a wrong
// name into a visible field, which is recoverable; a check that was wrong lets
// a request out, which is not.
enum class Detect {
    NotRun,      // nobody asked. The default, and it proposes nothing.
    NoRoute,     // no route out of this family at all -- for v6 that is a
                 // perfectly good state and not a failure
    Unmatched,   // the kernel named a source address no interface here claims
    Ambiguous,   // more than one device claims it. Not "pick the first": the
                 // whole value of this is that it does not guess.
    Found
};

const char* detect_name(Detect d);

// One interface's claim on one address, as `getifaddrs` reports it.
struct DeviceAddress {
    std::string device;
    std::string address;
};

// The result of a detection: a state and, only when that state is `Found`, a
// name. Two fields rather than an empty-means-no, because "no v6 route" and
// "two devices claim this address" are opposite problems and a caller that
// cannot tell them apart writes the wrong sentence for both.
struct DeviceFound {
    Detect      state = Detect::NotRun;
    std::string device;   // set only when `state == Found`
};

// Address -> device. `lo` is never the answer: a source address that only
// loopback claims means the packet was not going anywhere, which reads as
// `NoRoute` rather than as a device a person might paste into the Tunnel box.
DeviceFound device_for_address(const std::string& address,
                               const std::vector<DeviceAddress>& devices);

// ── Is the thing we just detected plausibly a tunnel ─────────────────────────
// The guard on the second setup step, and the reason the first step records a
// device name at all. If the device carrying traffic with the VPN *on* is the
// same one that carried it with the VPN *off*, the VPN is not up -- and writing
// that name into the Tunnel box would configure the app to bind to the user's
// ordinary connection and call it a tunnel.
//
// Note what the states do NOT claim. `Distinct` is not "the tunnel is up"; it
// is "not the one device we can prove it isn't". The proof that a tunnel works
// is a preflight, and this is a hint two steps before one.
enum class TunnelCheck {
    NoDevice,   // nothing was detected; there is nothing to say
    Unchecked,  // no baseline device recorded, so this cannot be checked at all
    NotUp,      // the device the baseline was recorded on. The tunnel is off.
    Distinct    // some other device -- consistent with a tunnel, not proof of one
};

const char* tunnel_check_name(TunnelCheck t);

TunnelCheck tunnel_check(const DeviceFound& detected, const std::string& naked_device);

// ── Which address is "the tunnel's own address" ──────────────────────────────
// `getifaddrs` hands back everything the interface has: a v4, maybe a ULA,
// usually a link-local. `egress_check` compares the bound address to the
// interface address, so BOTH SIDES MUST NAME THE SAME ONE -- and that agreement
// is the whole job of this function. It is not "the best address". It is the
// one address the binder binds to and the observer reports, chosen the same way
// twice.
//
// Link-local and loopback are never it (a socket bound to fe80:: reaches
// nothing off-link), and v4 is preferred over v6 because tunnels hand out a v4
// and that is what a bind to a tunnel usually means. Empty when there is
// nothing usable -- an interface that is up with no address cannot carry
// traffic, the shim will fail to bind to nothing, and `NotBound` is the honest
// verdict for it.
std::string iface_pick_address(const std::vector<std::string>& addresses);

// ── What the preflight echo said ─────────────────────────────────────────────
// The echo is a request to something that answers with the address it saw us
// come from. Its body is whatever that service felt like sending: a bare
// address and a newline, a scrap of JSON, or a whole HTML page with an ad on
// it.
//
// Exactly one distinct readable address in the body, or nothing. Two is not
// "pick the first" -- `1.0.0.1` is a perfectly valid address and also a version
// number, and an echo that needs disambiguating is an echo pointed at the wrong
// service. Nothing here means `ExitUnobserved`, which is a refusal with a fix,
// and that beats a confident answer scraped out of a paragraph.
std::string echo_read(const std::string& body);

// ── What the DNS canary said ─────────────────────────────────────────────────
// The canary resolves a name whose ANSWER IS THE ADDRESS OF WHOEVER ASKED --
// the authoritative server reports the querying resolver back to us. So
// attribution becomes address judgment, the same judgment made about the exit,
// which is why this file does not need a second vocabulary for it.
//
//   our own address answered      -> Leaked. The lookup went out naked.
//   a NO-TUNNEL RESOLVER answered -> Leaked. A resolver that answers with the
//                                    tunnel down, answering with it up, is the
//                                    lookup going around the tunnel.
//   a private address answered    -> Leaked. Something on the local network
//                                    answered, off-tunnel by definition.
//   nothing readable              -> Failed. Not evidence of safety.
//   any other public address      -> Clean.
//
// That last line is a deliberate loosening, and worth its paragraph: a
// provider's resolvers routinely egress from a different address than the exit
// node, so demanding that the canary's address match `accepted_exits` would
// refuse most real VPNs forever. The canary answers ONE question -- did the
// lookup go around the tunnel -- and it answers it by recognising the addresses
// that could only mean yes. The tighter identity check already exists for
// `DnsMode::Pinned`, where `ResolverMismatch` compares against a resolver the
// user named.
//
// ── Why the second baseline exists, and what it fixed ────────────────────────
// Until s9 this compared against `naked_exit` alone, which catches only the
// case where THIS MACHINE did the resolving. It does not catch the ordinary
// leak: the query escapes to the ISP's resolver, which egresses from an address
// nothing like the user's own, and an unfamiliar public address read as Clean.
//
// The baseline is a LIST because one sample is not the set -- a provider's
// recursives answer from a pool, and recognising R1 while waving R2 through
// would be a mode that looks verified. What this can honestly claim is "not a
// resolver we have ever seen answer you without a tunnel", and the strength of
// that claim is the number of times Record was pressed.
//
// Under `Proxied` that was tolerable -- the proxy is the guarantee and this
// merely corroborates. Under `DnsMode::SystemVerified` THE CANARY IS THE
// GUARANTEE, and shipping this as written in that role would have been a mode
// that looked verified and verified nothing.
//
// Both baselines are used ONLY TO FAIL, which is the property that keeps this
// safe to hand policy-derived strings: an empty or unreadable baseline removes
// a way to detect a leak and can never manufacture a pass. The policy layer is
// what refuses `SystemVerified` outright when the resolver baseline is absent,
// because whether a missing baseline is fatal depends on the mode, and this
// function does not know the mode and should not learn it.
Canary canary_read(const std::string& answered_by, const std::string& naked_exit,
                   const std::vector<std::string>& naked_resolvers);

// ── What the shim read ───────────────────────────────────────────────────────
// Raw, verbatim, and honest about gaps. Default-constructed this produces an
// observation that REFUSES, field by field, so a half-written observer fails
// closed rather than passing on silence.
struct ProbeReadings {
    // Where the routing table came from. Three states because "no such file"
    // and "could not read it" are not the same fact: a kernel booted with v6
    // disabled has no `/proc/net/ipv6_route` AND no way to leak over v6, and
    // refusing that machine would be refusing the safer configuration.
    enum class RouteSource {
        Unread,  // nobody looked. A wiring bug, and it fails closed.
        Absent,  // no such file: this kernel has no v6 at all
        Text     // read it; `routes_text` is what it said
    };

    // From getifaddrs.
    bool interface_present = false;
    bool interface_up      = false;   // IFF_UP and IFF_RUNNING, both
    std::vector<std::string> interface_addresses;

    // From getsockname on the socket we actually opened. Not "could we bind"
    // -- whether we did.
    bool        bound = false;
    std::string bound_address;

    RouteSource routes = RouteSource::Unread;
    std::string routes_text;

    // The preflight. `*_ran` false is a wiring bug rather than a network
    // condition, and reads as such.
    bool        echo_ran = false;
    std::string echo_body;        // PII: contains our public address
    bool        canary_ran = false;
    std::string canary_answered_by;   // the whoami answer: who asked, out there

    // The resolver the query was actually SENT to, as the shim configured it.
    // A different fact from the line above and they must not be conflated: a
    // pinned resolver at 10.7.0.1 inside the tunnel will have some recursive's
    // public address come back from the whoami, and reporting that as the
    // resolver would trip `ResolverMismatch` on a perfectly correct setup.
    // This one answers `Egress`'s question -- did the query go to the resolver
    // the user named -- and `canary_answered_by` answers the other one, whether
    // it went out through the tunnel at all. Empty under `Proxied`, where by
    // design we do not know and are not supposed to.
    std::string resolver_used;

    std::int64_t observed_at_s = 0;   // caller's monotonic seconds
};

// Readings -> observation. The whole translation, in one pure call.
//
// Takes the policy because an observer cannot observe "the tunnel" without
// being told which interface that is, and the canary cannot say "that was our
// own address" without the baseline. Nothing policy-derived in here can turn a
// refusal into a pass: `naked_exit` is only ever used to FAIL the canary, the
// same way `egress_check` only ever uses it to refuse.
EgressObservation observation_from(const ProbeReadings& r, const EgressPolicy& p);

}  // namespace delr::core
