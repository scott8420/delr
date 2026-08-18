#pragma once
#include "core/Case.hpp"

#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// core/Egress -- may this request leave, and out of which hole.
//
// The verification fetch is the point of this app, and it is also the one thing
// it does that touches the network. Checking a broker listing on a 45-day
// rhythm from your home address tells that broker exactly what it would like to
// know: that the person in the listing is watching the listing. So the fetch
// goes out through a tunnel or it does not go out at all.
//
// TWO IDEAS, and they are not the same idea:
//
//   Bind before you check, or the check is the leak. Binding the socket to the
//   tunnel's address makes a dead tunnel FAIL rather than quietly fall back to
//   the default route. That is a killswitch by construction: it holds for every
//   request, including the ones written after everybody stopped thinking about
//   it. A check that runs before the request is a snapshot; a bind is a
//   property.
//
//   The preflight answers the question binding cannot: IS THIS THE TUNNEL I
//   MEANT? A socket bound to a live interface that terminates somewhere you did
//   not intend is bound, and wrong.
//
// And the leak that survives both: DNS. Resolving a broker's hostname on a
// 45-day rhythm reveals the entire shape of the activity with zero HTTP
// requests escaping. Whoever answers that query learns the schedule and the
// roster. So the resolver is part of the policy, not part of the plumbing.
//
// ── What is in this file, and what is deliberately not ───────────────────────
// The POLICY, and nothing else: what a tunnel has to look like, what a
// preflight has to show, what counts as a refusal and what a refusal DOES to a
// case. No sockets, no resolver, no clock, no filesystem. Everything here is a
// pure function of a policy the user configured and an observation somebody
// else made, which means the whole of the leak-or-not decision is exercised in
// `delr --selftest` with no packet in the room.
//
// The socket layer lands underneath this later and its only job is to fill in
// an `EgressObservation` honestly. That inversion is the point: when the fetch
// arrives it cannot invent a new way to be allowed out, because being allowed
// out is decided here.
//
// ── PII WARNING ──────────────────────────────────────────────────────────────
// An address is PII. `naked_exit` in particular is the user's home IP -- the
// single identifier this entire app exists to keep away from brokers -- and it
// is stored here in the clear precisely so the app can recognise it and refuse.
// NOTHING IN THIS FILE LOGS, no verdict text ever contains an address, and
// `egress_log_ref()` exists so the safe thing is the easy thing. The same rule
// as `Case::url`, for the same reason.
// ─────────────────────────────────────────────────────────────────────────────
namespace delr::core {

// ── Addresses ────────────────────────────────────────────────────────────────
// Enough address vocabulary to make a judgment, and no more: this is not a
// network library, it is the four questions the policy actually asks.

// What kind of address this is. Named states rather than an `is_public` bool,
// because a private answer and an unparseable answer mean different things --
// the first says something intercepted the preflight, the second says the layer
// below is broken -- and both would flatten to "not public".
enum class AddrKind {
    Invalid,    // not an address we can read
    Loopback,   // 127.0.0.0/8, ::1
    Private,    // RFC1918, RFC4193 (fc00::/7), and CGNAT 100.64/10
    LinkLocal,  // 169.254/16, fe80::/10
    Public
};

const char* addr_kind_name(AddrKind k);
AddrKind    addr_kind(const std::string& addr);

// The COMPARISON form of an address: v4 unchanged, v6 fully expanded and
// lower-cased ("2001:db8::1" -> "2001:0db8:0000:...:0001"). Empty when the
// address is unreadable.
//
// Two forms for the same reason Intake keeps two forms of a URL: the string a
// human typed into the policy and the string the observing layer reported are
// both correct and need not match byte-for-byte, and an exit that fails to
// match because one side wrote `::1` and the other `0:0:0:0:0:0:0:1` would
// refuse a perfectly good tunnel. This form is never displayed.
std::string addr_canonical(const std::string& addr);

// True when two addresses are the same address. Invalid never equals anything,
// including another invalid -- "we could not read either of these" is not
// grounds for a match.
bool addr_same(const std::string& a, const std::string& b);

// ── How names get resolved ───────────────────────────────────────────────────
// A mode, not a hostname setting, because the failure this prevents is not
// "wrong resolver" but "the resolver you forgot about".
enum class DnsMode {
    Unset,    // nobody has decided. Refused: an undecided resolver is the system one.
    System,   // whatever /etc/resolv.conf says. ALWAYS REFUSED -- see below.
    Pinned,   // a named resolver, reached over the tunnel
    Proxied   // the proxy resolves (socks5h, not socks5). The name never leaves here.
};

const char* dns_mode_name(DnsMode m);
DnsMode     dns_mode_from(const std::string& s);

// System is a refusal rather than a setting. This is the one place the module
// takes an option away from the user on purpose: a tunnel with the host's own
// resolver behind it leaks the entire roster and schedule to the ISP while
// every HTTP request goes out clean, and it looks completely fine from inside
// the app. There is no flag to allow it. A privacy tool with a switch that
// silently defeats it does not have a switch, it has a bug with a label.

// ── Did the name lookup go where it was supposed to? ─────────────────────────
// The preflight's canary: resolve something whose answer we can attribute, and
// see who answered. Four states, because "no" splits three ways and the fixes
// differ -- not wired up, could not tell, and demonstrably went elsewhere.
enum class Canary {
    NotRun,   // the preflight did not do this. A wiring bug, not a network condition.
    Clean,    // the lookup went where policy says it should
    Failed,   // we could not attribute the answer. Not evidence of safety.
    Leaked    // something off-tunnel answered. The exact thing we are here to stop.
};

const char* canary_name(Canary c);

// ── The policy ───────────────────────────────────────────────────────────────
// What the user configured. Default-constructed, this refuses everything: an
// unconfigured policy means the user has not told us where the tunnel is, and
// the safe reading of "I don't know" is "no".
struct EgressPolicy {
    std::string interface_name;   // "wg0", "tun0" -- what we bind to

    // Exit addresses we accept. A LIST, not one address: providers rotate exit
    // nodes, and a policy pinned to a single address would refuse the whole app
    // the first time the provider did something normal. The UI's job is to
    // offer "trust this exit" when a new one shows up -- a deliberate act,
    // which is the difference between accepting a new exit and not noticing.
    std::vector<std::string> accepted_exits;

    // What we look like with the tunnel DOWN. The user's own address. Learned
    // once, deliberately, with the tunnel off; stored so that the most
    // important refusal in the file -- ExitNaked -- is decidable at all.
    // Never logged, never displayed, never sent anywhere.
    std::string naked_exit;

    DnsMode     dns = DnsMode::Unset;
    std::string resolver;   // required when dns == Pinned

    // How long a preflight pass is good for. A preflight is a fact about a
    // moment: tunnels drop between requests, and a pass from an hour ago is a
    // story about an hour ago. Short by default; the cost of re-running it is a
    // round trip and the cost of trusting a stale one is the whole point of the
    // app.
    std::int64_t preflight_ttl_s = 300;
};

// Guards the policy itself, in the house style: a problem per line, empty means
// clean. Same job as `roster_validate` -- a policy that cannot be satisfied is
// a bug in the CONFIG, and finding it at configure time beats finding it as a
// refusal on every fetch forever.
std::vector<std::string> egress_policy_validate(const EgressPolicy& p);

// ── The observation ──────────────────────────────────────────────────────────
// What the layer below measured. Today the selftest fills this in; tomorrow the
// socket code does. Nothing in here is inferred by this file -- if the observer
// did not look, the field says so and the verdict says so.
//
// Default-constructed, this REFUSES (nothing bound, nothing observed). Every
// field's default is the pessimistic reading, so a half-filled observation from
// a half-written observer fails closed rather than passing on silence.
struct EgressObservation {
    bool        interface_present = false;
    bool        interface_up      = false;
    std::string interface_address;   // the tunnel's own address

    // Did the socket actually bind, and to what. Not "can we reach the
    // interface" -- the bind is the killswitch, so the question is whether it
    // happened, not whether it would have worked.
    bool        bound = false;
    std::string bound_address;

    // A v6 default route that does not go through the tunnel. A v4-only tunnel
    // on a dual-stack host leaks every request the host prefers to make over
    // v6, which on a modern desktop is most of them, and the tunnel is up and
    // healthy the entire time.
    bool v6_default_offtunnel = false;

    std::string observed_exit;      // what the preflight's echo reported
    std::string observed_resolver;  // who answered the canary
    Canary      canary = Canary::NotRun;

    // When the PREFLIGHT was made, in the caller's monotonic seconds. It
    // stamps the far-end fields (exit, resolver, canary) and not the bind
    // fields, which the observer reads fresh at the call. The core has no
    // clock, here as everywhere: `Case` takes `today`, this takes `now_s`.
    std::int64_t observed_at_s = 0;
};

// ── The verdict ──────────────────────────────────────────────────────────────
// One named state per way this can be wrong, because "egress blocked" is
// unactionable and these are eighteen different fixes. This is the same
// judgment `UrlProblem` makes about a bad paste, applied to the network.
enum class Verdict {
    Pass,

    // Policy problems. Timeless -- they do not depend on any observation, and
    // no amount of re-running the preflight changes them.
    Unconfigured,     // no interface named
    DnsUnset,         // nobody decided how names resolve
    DnsSystem,        // the host resolver. Refused by design; see DnsMode.
    ResolverMissing,  // Pinned, with no usable resolver address
    ExitUnpinned,     // no accepted exit AND no naked baseline: the preflight
                      // has nothing to compare against, so it cannot answer its
                      // one question and passing it would be theatre.

    // Can anything leave off-tunnel -- the first question. Current state, not
    // preflight evidence, which is why it is judged before staleness.
    NoInterface,
    InterfaceDown,
    NotBound,
    BindMismatch,     // bound to something that is not the tunnel's address
    V6OffTunnel,

    // Our evidence about the far end is out of date -- or was never gathered.
    Stale,

    // Is this the tunnel I meant -- the second question.
    ExitUnobserved,   // the preflight produced no readable exit address
    ExitPrivate,      // the echo came back private or loopback: something in
                      // the middle answered, and we are not talking to who we
                      // think we are talking to
    ExitNaked,        // it is our own address. The tunnel is down. The refusal
                      // this whole file exists for.
    ExitUnexpected,   // public, not naked, not on the accepted list

    // Did the name lookup go with it -- the third question.
    CanaryNotRun,
    CanaryFailed,
    CanaryLeaked,
    ResolverMismatch  // something other than the pinned resolver answered
};

const char* verdict_name(Verdict v);

// The sentence a window shows. NEVER contains an address, a hostname or an
// interface's addressing -- a refusal is a thing the user screenshots when
// asking for help, and `naked_exit` is the last string in this program that
// should travel. Same rule as `url_problem_text()`, higher stakes.
const char* verdict_text(Verdict v);

bool verdict_clear(Verdict v);   // Pass, and only Pass

// ── The one call the fetch makes ─────────────────────────────────────────────
// Evaluated in the order the enum is written, and the ORDER IS ITSELF A
// JUDGMENT: the first thing wrong is the first thing to fix, so
//
//   policy problems first -- they are true regardless of what was observed and
//     no preflight can clear them;
//   then whether anything can leave off-tunnel at all, which is a fact about
//     the socket and the routes RIGHT NOW rather than a preflight result;
//   then staleness, because everything after this point is the preflight's
//     evidence, and if that evidence is old the honest answer is "look again"
//     rather than a confident story about the exit address as of an hour ago;
//   then the far end: is this the tunnel I meant, and did the lookup go with it.
//
// Exactly one verdict comes back. A list of everything wrong would be a more
// complete report and a worse instruction.
Verdict egress_check(const EgressPolicy& p, const EgressObservation& o,
                     std::int64_t now_s);

// The whole of the question at the call site: may this request leave?
inline bool egress_may_fetch(const EgressPolicy& p, const EgressObservation& o,
                             std::int64_t now_s) {
    return verdict_clear(egress_check(p, o, now_s));
}

// ── Where this meets the caseload ────────────────────────────────────────────
// `Reason::NoTunnel` has been sitting in `core::Case` since the schema was
// written, waiting for a producer. This is it.
//
// A refusal is NOT a check. It records Indeterminate/NoTunnel -- which never
// rounds to NotFound, the invariant the schema was built around -- and then
// does two things `apply_check` would not:
//
//   The failure streak does not move. `consecutive_failures` means "this
//   listing is hard to fetch", and our tunnel being down says nothing whatever
//   about the listing. Letting our own outage accumulate there would, weeks
//   later, read as broker hostility and could argue a case into Abandoned on
//   the strength of an expired VPN subscription.
//
//   It comes back soon, not in 45 days. The broker's recheck rhythm is the
//   answer to "how often is this worth looking at"; it is not the answer to
//   "when will our own network be fixed".
//
// `last_attempt` does move: we did try, and a run that refuses is a run.
Case apply_egress_refusal(const Case& c, const std::string& today,
                          int retry_days = 1);

// Safe log identifier -- "egress:<interface>/<verdict>". No exit address, no
// naked address, no resolver: an ip in a log file is the user's location in a
// log file. Anything that logs an egress decision logs THIS.
std::string egress_log_ref(const EgressPolicy& p, Verdict v);

}  // namespace delr::core
