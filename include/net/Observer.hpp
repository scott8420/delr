#pragma once
#include "core/Egress.hpp"
#include "core/Probe.hpp"

#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// net/Observer -- the syscall shim, and the thinnest thing that can still be
// honest.
//
// `core/Probe` took the judgment out of this file before it existed. Which of
// an interface's four addresses is "the tunnel's", whether a routing table line
// is a leak, whether a response body contains an address worth believing --
// all of that is decided in the core, pure, under the selftest. What is left
// here is: read four things, hand them over verbatim, and say plainly which
// ones could not be read.
//
// That is the whole design. This file cannot invent a new way to look safe,
// because it does not contain the word "safe". It contains `getifaddrs`, a
// `bind`, a `getsockname`, one file read and two round trips.
//
// ── Failing closed is the default, not a branch ──────────────────────────────
// A `ProbeReadings` default-constructs into a refusal, field by field. So every
// error path in this file is the same error path: leave the field alone and
// return. There is no `else` here that has to remember to be pessimistic --
// which is why the syscall layer got written second and not first.
//
// ── PII WARNING ──────────────────────────────────────────────────────────────
// `readings.echo_body` is a response containing this machine's public address
// and `canary_answered_by` is an address. `notes` is the only field in this
// file safe to display or log, and it is safe by construction: fixed sentences
// chosen from a short list, never interpolated with anything read off the
// network or out of the policy. Nothing in this file logs.
// ─────────────────────────────────────────────────────────────────────────────
namespace delr::net {

// ── Where the preflight asks its two questions ───────────────────────────────
// The two endpoints ARE THE POLICY'S, and they live in `core::EgressPolicy`
// with their defaults -- see the block there for why they stopped being
// literals in s9. This struct is the net layer's working copy of them, and it
// default-constructs EMPTY on purpose: an observer handed nothing observes
// nothing, and every field in this subsystem fails in that direction. Build one
// with `observer_config()` rather than by hand.
struct ObserverConfig {
    // Answers with the address it saw us arrive from. A bare address and a
    // newline is the ideal shape; `core::echo_read` will find one in a page of
    // HTML too, and refuses if it finds two.
    std::string echo_url;

    // Answers with the address of the RESOLVER that looked it up, which is a
    // different question from the line above and the one DNS leaks are made of.
    //
    // How it manages that is worth knowing, because it is what makes the answer
    // mean anything: it redirects to a randomly-named subdomain, so the lookup
    // cannot come from any cache, and the authoritative server for that name
    // records who asked. Redirects are followed by `net/Fetch`, which is a
    // requirement here rather than a convenience.
    //
    // Empty disables the canary, which is `CanaryNotRun`, which is a refusal.
    // That is the honest state for an unconfigured preflight and not a hole:
    // "we did not check whether the lookup leaked" must never read as "it
    // didn't".
    std::string canary_url;

    // The preflight runs on the tunnel and the tunnel may be slow. Separate
    // from a page fetch's budget because a broker timing out is a fact about
    // the broker, and this timing out is a fact about us.
    long timeout_s = 20;
};

// The policy's endpoints, as a config. One line, and it exists so that four
// call sites cannot each remember two field names differently -- the failure
// this prevents is a caller that copies `echo_url`, forgets `canary_url`, and
// silently turns the lookup check off in one window and not the others.
ObserverConfig observer_config(const core::EgressPolicy& p);

// ── What came back ───────────────────────────────────────────────────────────
// The readings, plus a short list of things a maintainer or a settings page
// should be told. `notes` are diagnostics, never evidence: nothing upstream
// reads them to decide anything, and `observation_from` never sees them. A note
// exists for the cases where the readings are correctly pessimistic but the
// REASON is a fact about the machine rather than about the network -- which is
// the difference between "your tunnel is down" and "your libcurl cannot do
// what your settings ask", and a user who cannot tell those apart is stuck.
struct Observation {
    core::ProbeReadings      readings;
    std::vector<std::string> notes;   // safe to display; contains no addresses
};

// Take the four readings. Needs the policy to know which interface is "the
// tunnel" and nothing else from it -- the same restriction `observation_from`
// operates under, for the same reason: nothing policy-derived may turn a
// refusal into a pass.
//
// `now_s` is the caller's monotonic seconds, stamped into the readings. The
// shim has no clock, here as everywhere in this codebase.
Observation observe(const core::EgressPolicy& p, const ObserverConfig& cfg,
                    std::int64_t now_s);

// The whole preflight in one call: observe, then translate. The pairing that
// every caller wants and nobody should re-type, because forgetting
// `observation_from` and hand-rolling the translation is exactly how a second,
// wrong judgment layer gets born.
inline core::EgressObservation preflight(const core::EgressPolicy& p,
                                         const ObserverConfig& cfg,
                                         std::int64_t now_s,
                                         Observation* detail = nullptr) {
    Observation obs = observe(p, cfg, now_s);
    const core::EgressObservation out = core::observation_from(obs.readings, p);
    if (detail) *detail = std::move(obs);
    return out;
}

// ── The pieces, exposed for the selftest and for a settings page ─────────────
// Impure, all of them; they are here so a settings window can say "wg0 is up
// and has these addresses" without running a preflight, and so a future session
// debugging a bind has something smaller than `observe()` to call.

// Everything getifaddrs knows about one interface.
struct IfaceReading {
    bool present = false;
    bool up      = false;   // IFF_UP and IFF_RUNNING, both
    std::vector<std::string> addresses;
};
IfaceReading iface_read(const std::string& interface_name);

// Every interface this machine has, by name, sorted and de-duplicated. A
// settings window needs it because "wg0" is a guess: a user whose tunnel comes
// up as `proton0` or `tun0` and who types `wg0` gets `NoInterface`, which reads
// as "your VPN is off" and sends them to fix the wrong thing.
//
// Names only. An interface NAME is not an address and is safe to show; the
// addresses behind it are not, and `iface_read` is where you go if you need
// them for a bind.
std::vector<std::string> iface_list();

// ── Asking the kernel which way out it would take ────────────────────────────
// The setup path's one new reading, and the thing that lets two buttons replace
// two boxes a user had to guess at.
//
// NOTHING IS SENT. `connect()` on a datagram socket performs a route lookup and
// assigns a source address; no packet is generated and no host is contacted.
// That property is why it is acceptable in this program at all -- a privacy
// tool that quietly opened a connection to a third party in order to work out
// its own network layout would be doing a smaller version of the thing it
// exists to prevent -- and it is why the destination is
// `192.0.2.1` / `2001:db8::1`: reserved-for-documentation addresses (RFC 5737,
// RFC 3849) that no real service occupies, so even the intent is unambiguous
// to anyone reading the code or a packet capture that will never contain one.
//
// The answer is a source ADDRESS; `core::device_for_address` turns it into a
// device name. See the block above it in `core/Probe.hpp` for why this is asked
// of the kernel instead of read out of `/proc/net/route` -- in short, wg-quick
// and OpenVPN both capture traffic without changing the main table's default,
// so the file would name the physical connection on a perfectly tunnelled
// machine.
//
// A HINT, NEVER A CHECK. Nothing here feeds an observation or a verdict: it
// proposes a name into a field a person can see and change. `egress_check`
// decides exactly what it decided before this function existed.
core::DeviceFound route_device(bool v6);

// ── Recording the baseline, which is the one deliberately naked request ──────
// `EgressPolicy::naked_exit` is what this machine looks like with the tunnel
// DOWN, and `Verdict::ExitNaked` -- the refusal the whole module exists for --
// is undecidable without it. It cannot be learned through the tunnel, by
// definition, so learning it means one request that does not go through the
// tunnel. This function is that request and it is the ONLY one in the program.
//
// It is not a hole in the killswitch, and the reason is that it is still
// bound: the caller names an ordinary interface (`eth0`, `wlan0`), the address
// is picked by the same `core::iface_pick_address` used everywhere else, and
// an interface with no usable address returns empty rather than falling back
// to the default route. So this cannot leave by a path the user did not name,
// which is the property that matters -- what it cannot be is *private*, and it
// is not pretending to be.
//
// No proxy and no resolver pinning are applied, deliberately: a baseline
// recorded through a proxy is the proxy's address, which would be recorded as
// the user's home address and would then make `ExitNaked` fire on a working
// tunnel forever.
//
// What the far end learns is this machine's public address, which is the one
// thing it already knew by virtue of being asked. Nothing else is sent.
//
// ── TWO baselines, one press ─────────────────────────────────────────────────
// s9 added the second: who ANSWERS THE LOOKUP with the tunnel down. It is
// gathered here rather than in a button of its own because it is the same fact
// about the same moment -- "what this computer looks like without the tunnel"
// -- and a second button is a second thing to forget, recorded at a second
// moment when the tunnel might be back up.
//
// The lookup half is the canary endpoint asked over the naked binding, which is
// the same request `observe()` makes and the reason nothing new had to be
// invented for it. It uses no proxy and no pinned resolver, for the same reason
// the exit half does not: a baseline recorded through either would record the
// wrong machine's answer and then fail every future check forever.
//
// The two halves fail INDEPENDENTLY. An exit recorded with no resolver is
// still worth keeping -- `Proxied` and `Pinned` never needed the second one --
// so each is reported as it came back and the caller is told which is missing.
struct BaselineResult {
    std::string              address;    // the naked exit. Empty on failure.
    std::string              resolver;   // who answered the naked lookup. Empty on failure.
    std::vector<std::string> notes;      // safe to display; contains no addresses
};
BaselineResult baseline_read(const std::string& interface_name,
                             const ObserverConfig& cfg);

// Open a socket, bind it to `address`, and report what getsockname says. Not
// "would this work" -- it opens the socket and binds it, because the bind IS
// the killswitch and a rehearsal of one proves nothing. Empty return means the
// bind failed, which is `NotBound`, which is a refusal.
std::string bind_probe(const std::string& address);

// Read /proc/net/ipv6_route. Three states, because a kernel with no v6 at all
// and a file we failed to read are opposite facts wearing the same shape.
core::ProbeReadings::RouteSource routes_read(std::string* text_out,
                                             const std::string& path = "/proc/net/ipv6_route");

}  // namespace delr::net
