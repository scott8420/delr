#include "netcheck.hpp"

#include "core/Egress.hpp"
#include "core/Probe.hpp"
#include "net/Fetch.hpp"
#include "net/Observer.hpp"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// `delr --netcheck [<interface>]` -- run one real preflight and say what
// happened.
//
// Written in s6 because the settings surface did not exist and the preflight
// had nowhere to be run from. The window exists now, and this stays -- with a
// second mode, which is the more useful one: with no arguments it runs THE
// SAVED POLICY.
//
// That is the tool for the gap between the two channels of evidence. The window
// can say "configured" and a fetch can still refuse, and the difference is
// either the file or the tunnel. This answers which, from a terminal, with no
// display, in output that is safe to paste into a bug report.
//
// The ad-hoc mode stays because it answers a question the saved mode cannot:
// what is my tunnel actually called, before there is anything to save.
//
// ── It prints no addresses. None. ────────────────────────────────────────────
// Not the exit, not the tunnel's own, not the resolver, not the count of a
// prefix. This is the output somebody pastes into a bug report when the check
// will not run, and `naked_exit` is the last string in this program that should
// travel. Same discipline as `egress_log_ref()`: yes/no, named states, and the
// verdict's own sentence -- all of which were written to be safe to say out
// loud.
// ─────────────────────────────────────────────────────────────────────────────
namespace delr::netcheck {
namespace {

const char* yn(bool b) { return b ? "yes" : "no"; }

const char* route_name(core::ProbeReadings::RouteSource s) {
    switch (s) {
        case core::ProbeReadings::RouteSource::Unread: return "unread";
        case core::ProbeReadings::RouteSource::Absent: return "no ipv6 on this kernel";
        case core::ProbeReadings::RouteSource::Text:   return "read";
    }
    return "unread";
}

// The header block: what this build can do, and against what. Same three lines
// for both modes, because the first question in a bug report is always which
// of these is not what the reporter thought.
void preamble(const core::EgressPolicy& p) {
    std::printf("  backend            %s\n",
                net::fetch_available() ? net::fetch_backend().c_str()
                                       : "none (built without libcurl)");
    std::printf("  can pin a resolver %s\n", yn(net::fetch_can_pin_dns()));
    std::printf("  interface          %s\n",
                p.interface_name.empty() ? "(none named)" : p.interface_name.c_str());
    std::printf("  name lookups       %s\n", core::dns_mode_name(p.dns));
    // Which ENDPOINTS, in the one form that is safe and still useful: whether
    // they are the ones delr ships with. They became settings in s9, so "the
    // canary is dead" and "the canary was pointed somewhere else" are now two
    // different bug reports and this is the line that tells them apart. The
    // urls themselves are third-party hostnames rather than user data, but
    // printing them would put an arbitrary user-typed string into output whose
    // whole promise is that it is safe to paste.
    const core::EgressPolicy shipped;
    std::printf("  address check      %s\n",
                p.echo_url.empty() ? "(none configured)"
                : p.echo_url == shipped.echo_url ? "default" : "custom");
    std::printf("  lookup check       %s\n",
                p.canary_url.empty() ? "(none configured -- refuses everything)"
                : p.canary_url == shipped.canary_url ? "default" : "custom");
    std::printf("\n");

    // ── what the setup path would detect right now ───────────────────────────
    // Printed here so that the detection s11 put behind two buttons has a TRACE
    // channel as well as a visual one. The window shows a name in a box, which
    // is where a wrong reading would be found late and blamed on the box; this
    // is where it can be found in one command, before anybody trusts a field
    // that filled itself in.
    //
    // Device NAMES only, which is the same rule as everywhere else in this file
    // -- `iface_list()` already prints them in the settings window and a name
    // is not an address. Nothing here runs a check or feeds one: it reports a
    // hint.
    const core::DeviceFound d4 = net::route_device(false);
    const core::DeviceFound d6 = net::route_device(true);
    std::printf("would leave by\n");
    std::printf("  ipv4               %s%s%s\n", core::detect_name(d4.state),
                d4.device.empty() ? "" : "  ", d4.device.c_str());
    std::printf("  ipv6               %s%s%s\n", core::detect_name(d6.state),
                d6.device.empty() ? "" : "  ", d6.device.c_str());
    std::printf("\n");
}

// Run it, print it, and return the exit code. NO ADDRESSES -- see the block at
// the top. Yes/no, counts, named states, and the verdict's own sentence, every
// one of which was written to be safe to say out loud.
int observe_and_report(const core::EgressPolicy& p) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();

    net::Observation detail;
    const core::EgressObservation o =
        net::preflight(p, net::observer_config(p), now, &detail);
    const core::ProbeReadings& r = detail.readings;

    std::printf("readings\n");
    std::printf("  interface present  %s\n", yn(r.interface_present));
    std::printf("  interface up       %s\n", yn(r.interface_up));
    std::printf("  addresses found    %zu\n", r.interface_addresses.size());
    std::printf("  bound              %s\n", yn(r.bound));
    std::printf("  ipv6 route table   %s\n", route_name(r.routes));
    std::printf("  echo ran           %s\n", yn(r.echo_ran));
    std::printf("  canary ran         %s\n", yn(r.canary_ran));
    std::printf("\n");

    std::printf("observation\n");
    std::printf("  exit readable      %s\n", yn(!o.observed_exit.empty()));
    std::printf("  v6 off-tunnel      %s\n", yn(o.v6_default_offtunnel));
    std::printf("  canary             %s\n", core::canary_name(o.canary));
    std::printf("\n");

    const core::Verdict v = core::egress_check(p, o, now);
    std::printf("verdict              %s\n", core::verdict_name(v));
    std::printf("  %s\n", core::verdict_text(v));

    if (!detail.notes.empty()) {
        std::printf("\nnotes\n");
        for (const auto& n : detail.notes) std::printf("  - %s\n", n.c_str());
    }

    // Zero only on a pass, so this is usable from a script that wants to know
    // whether a check would run at all.
    return core::verdict_clear(v) ? 0 : 1;
}

}  // namespace

std::string policy_path() {
    if (const char* env = std::getenv("DELR_EGRESS")) return env;
    // Beside the working directory, like the roster and the caseload. The app
    // resolves it by calling THIS, so there is one definition of the default
    // rather than two that can drift.
    return "data/egress.json";
}

int run_saved() {
    std::string err;
    const core::EgressPolicy p = core::egress_policy_load(policy_path(), &err);

    std::printf("delr netcheck -- the saved policy\n");
    std::printf("  file               %s\n", policy_path().c_str());
    if (!err.empty()) {
        std::printf("  read               FAILED: %s\n", err.c_str());
        return 2;
    }
    preamble(p);

    // What the policy says wrong about ITSELF, before a socket is opened.
    // These are the problems no amount of re-running changes, and putting them
    // first is the ordering the verdict enum already argues for.
    const auto problems = core::egress_policy_validate(p);
    std::printf("policy\n");
    std::printf("  problems           %zu\n", problems.size());
    for (const auto& x : problems) std::printf("    - %s\n", x.c_str());
    // WHETHER a baseline exists, never what it is. The resolver half is a
    // COUNT rather than a yes/no, because with `system-verified` the number IS
    // the strength of the mode -- one sampled resolver is one resolver's worth
    // of protection against a provider that answers from several -- and a
    // count is not an address.
    std::printf("  own address known  %s\n", yn(!p.naked_exit.empty()));
    std::printf("  own resolvers seen %zu%s\n", p.naked_resolvers.size(),
                core::naked_resolver_known(p) ? "" : "  (none usable)");
    std::printf("  trusted exits      %zu\n", p.accepted_exits.size());
    std::printf("\n");

    return observe_and_report(p);
}

int run(const std::string& interface_name, const std::string& proxy) {
    core::EgressPolicy p;
    p.interface_name = interface_name;

    // A proxy is how a real run will go out, and without one the DNS mode has
    // to be something. Proxied either way: with a proxy it can pass, without
    // one it lands on ProxyMissing, which is correct and still exercises every
    // reading above the far end.
    p.dns = core::DnsMode::Proxied;
    p.proxy = proxy;

    std::printf("delr netcheck\n");
    preamble(p);
    return observe_and_report(p);
}

}  // namespace delr::netcheck
