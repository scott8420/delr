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
// case. No sockets, no resolver, no clock. Everything that JUDGES here is a
// pure function of a policy the user configured and an observation somebody
// else made, which means the whole of the leak-or-not decision is exercised in
// `delr --selftest` with no packet in the room.
//
// The pump at the bottom is the one filesystem touch, and it is the same
// exception `core/Broker` and `core/Case` already make: read bytes, write
// bytes, decide nothing. No judgment function in this file opens a file and no
// pump in this file reaches an opinion.
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
    Unset,           // nobody has decided. Refused: an undecided resolver is the system one.
    System,          // whatever /etc/resolv.conf says, unchecked. ALWAYS REFUSED -- see below.
    SystemVerified,  // the same resolver, with the canary made load-bearing. See below.
    Pinned,          // a named resolver, reached over the tunnel
    Proxied          // the proxy resolves (socks5h, not socks5). The name never leaves here.
};

const char* dns_mode_name(DnsMode m);
DnsMode     dns_mode_from(const std::string& s);

// ── System and SystemVerified are two values because they are two claims ─────
//
// `System` is a refusal, and stays one. It means "use whatever this computer
// uses and do not look": a tunnel with the host's own resolver behind it can
// leak the entire roster and schedule to the ISP while every HTTP request goes
// out clean, and from inside the app it looks completely fine. There is no flag
// to allow it. A privacy tool with a switch that silently defeats it does not
// have a switch, it has a bug with a label.
//
// `SystemVerified` is the same resolver with evidence attached, and it exists
// because the blanket refusal was refusing the ORDINARY CASE. With a VPN
// connected the system resolver IS the provider's, inside the tunnel; that is
// not a leak, it is a correctly configured VPN. Refusing it made this app
// unusable for anyone whose provider does not publish a SOCKS5 endpoint, which
// is most people who own a VPN account.
//
// What makes the difference real rather than cosmetic is that under this mode
// THE CANARY IS THE GUARANTEE. `Proxied` is guaranteed by the proxy and merely
// corroborated by the canary; here there is nothing else, so the mode requires
// a recorded `naked_resolver` and refuses without one
// (`ResolverBaselineMissing`), and every canary state except `Clean` is a hard
// refusal. It is the weaker mode and the window says so in words.
//
// The two values also keep the file honest about its own history: a
// hand-edited `"dns": "system"` must not silently upgrade itself into the
// permissive mode. It loads as `System`, and it is refused by name.

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

    // The OTHER devices that are also this tunnel.
    //
    // s6 assumed a tunnel is one interface, every hermetic check agreed, and
    // one real VPN broke it the first time anybody pointed the app at one:
    // Surfshark's WireGuard carries v4 on `surfshark_wg` and v6 on
    // `surfshark_ipv6`. The v6 default route goes via a device whose name is
    // not `interface_name`, `v6_default_route` reads that as a route around the
    // tunnel, and a correctly-tunnelled machine is refused with V6OffTunnel.
    //
    // ── Why this is a separate field and not just a wider `interface_name` ───
    // BINDING AND ROUTE-CHECKING ARE DIFFERENT QUESTIONS, and they only shared
    // a field while one name happened to answer both. "Where do I send this"
    // has exactly one answer -- CURLOPT_INTERFACE takes one device and there is
    // no meaning to binding a socket to two. "Is my v6 default inside my
    // tunnel" legitimately has several, because a provider may split a tunnel
    // across devices and that is not a leak.
    //
    // So `interface_name` stays singular and load-bearing for the bind, and
    // this list widens ONLY the route question. Nothing here is ever bound to,
    // and a name in this list grants no capability -- the worst a wrong entry
    // can do is stop V6OffTunnel firing on a route that really is outside the
    // tunnel, which is why it is a list the user types rather than anything
    // inferred from a prefix. `surfshark_*` was the cheap fix and it is a guess
    // wearing a rule: a device named to match walks straight through it.
    std::vector<std::string> tunnel_devs;

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

    // WHICH DEVICE the baseline above was recorded on. Not a setting -- a
    // record of an act, written only when a Record succeeds and cleared when
    // the baseline it belongs to is forgotten.
    //
    // It buys two things, both of them setup-path rather than check-path. It
    // survives a restart, so the box a user had to guess at on first run comes
    // back filled on every run after. And it is the ONLY thing the app can
    // compare a detected tunnel device against: if the device carrying traffic
    // with the VPN on is the device the baseline was recorded on with the VPN
    // off, the VPN is not up, and writing that name into `interface_name` would
    // configure the app to bind to the user's ordinary connection and call it a
    // tunnel. See `core::tunnel_check`.
    //
    // NOT PII, unlike its two neighbours. An interface name is not an address
    // and is shown in this window already -- `iface_list()` prints the whole
    // set of them. It rides in the same 0600 file because it belongs to the
    // same act, not because it needs the protection.
    //
    // Nothing in `egress_check` reads it. A wrong value here cannot let a
    // request out; the worst it does is decline to warn about a tunnel that is
    // not up, which is the state the app was in before this field existed.
    std::string naked_device;

    // The other half of the same baseline: WHO ANSWERS THE LOOKUP with the
    // tunnel down. Recorded in the same deliberate act as `naked_exit`, on the
    // same naked request, and used for exactly one thing -- to fail the canary
    // when one of them answers with the tunnel up.
    //
    // Without this, the canary could only catch a lookup that THIS MACHINE
    // answered, which is the narrow case. The ordinary leak escapes to the
    // ISP's resolver, whose address is nothing like the user's own and reads
    // as a perfectly good public answer. Under `Proxied` that weakness was
    // tolerable because the proxy was the guarantee; under `SystemVerified`
    // the canary IS the guarantee, and a mode that looks verified and verifies
    // nothing is worse than the refusal it replaced.
    //
    // ── A LIST, and for a different reason than `accepted_exits` ─────────────
    // The exit list is plural because providers rotate exits and a policy
    // pinned to one would refuse a working tunnel. This one is plural because
    // ONE SAMPLE IS NOT THE SET: a provider's recursive resolvers routinely
    // answer from a pool, so a baseline that captured R1 would wave a leak
    // through R2 while looking perfectly verified. The two lists therefore fail
    // in opposite directions -- a missing exit costs a false refusal, a missing
    // resolver costs a missed leak -- which is why recording APPENDS here
    // instead of replacing, and why pressing Record more than once with the
    // tunnel off is a documented thing to do rather than a mistake.
    //
    // It is still not a proof. A pool nobody sampled is a pool nobody knows
    // about, and the honest claim for this mode is "every resolver we have ever
    // seen answer you without a tunnel", never "any resolver that is not your
    // provider's".
    //
    // PII, exactly like `naked_exit`: recorded, never displayed, never logged.
    // Only the COUNT is ever said out loud.
    std::vector<std::string> naked_resolvers;

    DnsMode     dns = DnsMode::Unset;
    std::string resolver;   // required when dns == Pinned

    // The SOCKS proxy, required when dns == Proxied: "socks5h://host:port".
    //
    // The 'h' is not a spelling preference and `proxy_url_ok` refuses without
    // it. `socks5://` resolves the name HERE and sends the address onward;
    // `socks5h://` sends the name and the far end resolves it. Same wire
    // protocol, same working app, and one of them leaks the entire check
    // roster to whoever answers this machine's lookups while every byte of
    // HTTP goes out clean through the proxy. That is DnsMode::System wearing a
    // proxy's clothes, and it is refused for the same reason.
    std::string proxy;

    // How long a preflight pass is good for. A preflight is a fact about a
    // moment: tunnels drop between requests, and a pass from an hour ago is a
    // story about an hour ago. Short by default; the cost of re-running it is a
    // round trip and the cost of trusting a stale one is the whole point of the
    // app.
    std::int64_t preflight_ttl_s = 300;

    // ── Where the preflight asks its two questions ───────────────────────────
    // These were literals in `net::ObserverConfig` for three sessions and the
    // deferral expired the moment `SystemVerified` landed. Under `Proxied` a
    // dead canary endpoint is an annoyance -- the proxy is still the guarantee.
    // Under `SystemVerified` the canary is the ONLY guarantee, so a third party
    // going away, changing its output, or starting to return an ad becomes
    // every check refusing, with no way to fix it short of a rebuild.
    //
    // They live in the POLICY rather than in the net layer because they are
    // user configuration that has to persist and has to be editable, and
    // because putting the defaults here gives them one home instead of two that
    // can drift. `net::ObserverConfig` defaults to empty and is built from
    // these; an observer handed nothing observes nothing, which is the failure
    // direction this codebase already fails in.
    //
    // Neither endpoint is TRUSTED with anything. The echo's answer is compared
    // against a baseline the user recorded, and the canary's is only ever used
    // to FAIL. A hostile endpoint can refuse checks; it cannot permit one.
    //
    // An empty `canary_url` disables the canary, which is `CanaryNotRun`, which
    // is a refusal -- and under `SystemVerified` that is the whole mode gone.
    // "We did not check whether the lookup leaked" must never read as "it
    // didn't".
    std::string echo_url   = "https://api.ipify.org";
    std::string canary_url = "https://edns.ip-api.com/json";
};

// Is this a proxy we can use without leaking the lookup? "socks5h://host:port",
// and nothing else -- see EgressPolicy::proxy for why the 'h' is load-bearing.
// A bare host, an http proxy, or a socks5 without the 'h' are all false.
bool proxy_url_ok(const std::string& s);

// Guards the policy itself, in the house style: a problem per line, empty means
// clean. Same job as `roster_validate` -- a policy that cannot be satisfied is
// a bug in the CONFIG, and finding it at configure time beats finding it as a
// refusal on every fetch forever.
std::vector<std::string> egress_policy_validate(const EgressPolicy& p);

// Is there at least one usable no-tunnel resolver recorded? The precondition
// `SystemVerified` stands on, defined ONCE because four callers ask it -- the
// validator, `egress_check`, `--netcheck` and the settings window -- and four
// hand-rolled loops are four chances for the window to say "recorded" about a
// policy the check refuses.
//
// A recorded address that is not public does not count. A resolver on the local
// network is this computer's own resolver by another name, and a baseline made
// of one would fail every lookup rather than the leaked ones.
bool naked_resolver_known(const EgressPolicy& p);

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
    ProxyMissing,     // Proxied, with no usable socks5h proxy. Same shape as
                      // the line above: the mode names where lookups go and
                      // the address of that somewhere is missing.
    ResolverBaselineMissing,  // SystemVerified with no recorded `naked_resolver`.
                      // Same shape again, one layer over: this mode names no
                      // resolver at all, so the only thing standing between it
                      // and `System` is the canary, and the canary cannot
                      // recognise a leak without knowing what the leak looks
                      // like. The mode is its evidence; absent the evidence
                      // there is no mode.
    CanaryDisabled,   // no canary endpoint configured, in any mode. The lookup
                      // check was switched OFF rather than failed, which is a
                      // policy problem and not a network one -- no amount of
                      // re-running finds an endpoint nobody named. It has
                      // always produced a refusal (as `CanaryNotRun`); it only
                      // became reachable-on-purpose when the endpoints became
                      // a setting, and "you cleared this box" and "the
                      // preflight never ran" are two different sentences.
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

// ── Persistence pump ─────────────────────────────────────────────────────────
// Encode and decode in ONE place so a write cannot skew from its read, exactly
// as `roster_load`/`roster_save` and `caseload_load`/`caseload_save`. The
// CALLER resolves the path: the UI does the XDG work and hands in a plain
// string, so this file still never learns what a desktop is.
//
// ── FIDELITY IS THE BAR, AND IT INCLUDES THE BAD VALUES ──────────────────────
// A hand-edited file saying `"dns": "system"` loads as `DnsMode::System` and is
// then refused, loudly and by name, by `egress_policy_validate` and
// `egress_check`. It does NOT quietly become `Unset`. Both refuse, so the app
// is equally safe either way -- but they refuse with different sentences, and
// "the host resolver leaks every hostname you check" is the sentence that
// tells the truth about what is in the file. A loader that sanitises its input
// hands the user a fixed problem and the wrong explanation.
//
// Unrecognised values still fall to `Unset` via `dns_mode_from`, because there
// is no honest sentence to say about a mode nobody has ever heard of.
//
// ── PII WARNING, AND IT IS THE WHOLE FILE ────────────────────────────────────
// This writes `naked_exit` -- the user's home address, the one identifier this
// entire app exists to keep away from brokers -- to disk in the clear. That is
// not comfortable and it is not hidden: the policy cannot decide `ExitNaked`
// without it, and a baseline the user has to re-record on every launch is a
// baseline nobody records. `egress_policy_save` therefore creates the file
// mode 0600 BEFORE it writes a byte, rather than chmod-ing a world-readable
// file afterwards, because the gap between those two is a real gap. Encryption
// at rest is s9-s10's problem for the profile and this file rides along with
// it when it lands; until then 0600 and a comment is what is true.
//
// load is first-run tolerant: a missing file yields a DEFAULT-CONSTRUCTED
// policy, which is the policy that refuses everything. "We have not been
// configured" and "we are configured to allow nothing" are the same state here
// on purpose, and it is the safe one.
EgressPolicy egress_policy_load(const std::string& file, std::string* error = nullptr);
bool         egress_policy_save(const std::string& file, const EgressPolicy& p);

}  // namespace delr::core
