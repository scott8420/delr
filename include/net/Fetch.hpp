#pragma once
#include "core/Egress.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// net/Fetch -- the one thing this app does that touches the network.
//
// Everything about WHETHER a request may leave was decided in `core/Egress`,
// pure, months before a socket existed. This file is the other half of that
// bargain and it is deliberately the boring half: open a handle, configure it
// the way the policy says, GET one page, hand back a status and a body. It
// makes no judgments. It cannot decide it is allowed out, it cannot decide what
// the page means, and both of those are somebody else's finished module.
//
// ── Why this is not in delr_core ─────────────────────────────────────────────
// `delr_core` has no libcurl on its link line, the same way it has no gtkmm:
// the seam is enforced by the build graph rather than by everyone remembering.
// `delr_net` sits above it, links libcurl, and still has no UI on its line
// either -- so a stray `#include <gtkmm.h>` under `net/` fails to compile, and
// the fetch stays reachable from a headless selftest.
//
// `delr_net` builds even with no libcurl present. Without it every call here
// returns `NotBuilt` and says so, which keeps the target list from forking and
// keeps `fetch_url_ok`, the error vocabulary and the egress gate exercisable on
// a machine with nothing installed.
//
// ── The killswitch is structural, here too ───────────────────────────────────
// Every handle this file opens is bound to the tunnel's address before it is
// used, INCLUDING the observer's own preflight (see `fetch_unchecked`). There
// is no code path in this file that opens an unbound handle. If the tunnel is
// gone the bind fails and the request does not happen -- rather than quietly
// going out the other way, which is what a check-then-connect would do.
//
// ── PII WARNING ──────────────────────────────────────────────────────────────
// A `FetchResult` holds the body of a broker page fetched about the user, and a
// `FetchRequest` holds the listing URL. Both are PII of the sharpest kind: the
// url is documented as such in `core::Case` and the body is the listing itself.
// Nothing in this file logs. The error vocabulary is an enum precisely so that
// anything that DOES log has only `fetch_error_name()` to reach for -- curl's
// own message strings carry hostnames and addresses and are never surfaced.
// ─────────────────────────────────────────────────────────────────────────────
namespace delr::net {

// True when this build has libcurl under it. False makes every fetch below
// return `NotBuilt`; nothing else in the app changes shape.
bool fetch_available();

// The curl version string, for a settings page and a bug report. Empty when
// unavailable. Contains no user data.
std::string fetch_backend();

// ── Can this libcurl pin a resolver? ─────────────────────────────────────────
// `CURLOPT_DNS_SERVERS` needs a libcurl built against c-ares, and the stock
// Debian/Ubuntu build is not: it returns `CURLE_NOT_BUILT_IN`. That is a fact
// about the machine, not about the policy, and it is checked at runtime rather
// than assumed either way.
//
// It matters because the failure mode of getting this wrong is the worst one in
// the app: setting a pinned resolver, having the option quietly refused, and
// resolving every broker hostname through the host's own resolver while the
// HTTP side goes out through the tunnel looking perfect. `DnsMode::System` is a
// refusal by design in `core::Egress`; arriving there by accident must be a
// refusal too. So a `Pinned` policy on a libcurl that cannot pin returns
// `DnsPinUnavailable` and fetches nothing.
bool fetch_can_pin_dns();

// ── The request ──────────────────────────────────────────────────────────────
struct FetchRequest {
    std::string url;                       // PII: the listing

    long connect_timeout_s = 15;
    long timeout_s         = 45;

    // Redirects are followed, capped, and restricted to http/https. Following
    // them is not a risk to the verdict: a redirect to a login wall or a parked
    // domain lands on a page whose FINGERPRINT will not match, and
    // `Unfingerprinted` is exactly the right answer to "we ended up somewhere
    // else". Refusing to follow would instead produce a 301 with an empty body,
    // which `page_check` would have to guess about.
    long max_redirects = 5;

    // A hostile page is not a reason to eat memory. Past this the fetch stops
    // and reports `TooLarge` rather than handing back what it managed to read
    // -- a truncated body is the one input that could turn a live listing into
    // a clean absence, because the presence marker may be in the part we never
    // saw. A partial page is unreadable, not empty.
    std::size_t max_bytes = 4u * 1024u * 1024u;

    // Empty uses `fetch_default_user_agent()`.
    std::string user_agent;
};

// A broker serves a bot a different page than it serves a person, and the page
// rules were written by a person looking at the page. A default that gets us
// the markup a maintainer actually read is what makes those rules mean
// anything; announcing ourselves as a script gets us a challenge page and a
// week of `Unfingerprinted`. This is one public page about oneself every 45
// days, which is the least abusive traffic on the internet.
const char* fetch_default_user_agent();

// ── What went wrong, in the vocabulary of what to do about it ────────────────
// Named states rather than a curl code, for the same reason `Verdict` is named
// states rather than a bool: these are different fixes. A `Refused` is the
// user's settings, a `BindFailed` is the tunnel dropping mid-run, a `TooLarge`
// is ours, and a `Tls` on a broker's own domain is a story worth telling.
enum class FetchError {
    None,

    NotBuilt,           // no libcurl in this build
    BadUrl,             // not an http/https url we will touch
    Refused,            // the egress policy said no -- see FetchResult::verdict
    DnsPinUnavailable,  // policy pins a resolver; this libcurl cannot. See above.
    BindFailed,         // could not bind to the tunnel. The killswitch firing.
    ProxyFailed,        // the socks5h proxy refused or was not there
    Resolve,            // the name did not resolve
    Connect,            // no connection to the host
    Tls,                // certificate or handshake
    Timeout,
    TooLarge,           // past max_bytes; the body is discarded, not truncated
    Protocol,           // curl could not make sense of the response
    Other
};

const char* fetch_error_name(FetchError e);   // log-safe, no user data
const char* fetch_error_text(FetchError e);   // the sentence a window shows

// ── Whose problem is it ──────────────────────────────────────────────────────
// `core::PageRules` already answers this question for a page verdict -- a
// missing rule is ours, a 403 is theirs, and only theirs may move the failure
// streak. This is the same question one layer lower, where the fetch never got
// far enough for there to be a page to judge.
//
// True for the failures that are facts about THIS MACHINE -- the policy, the
// tunnel, the proxy, the library -- and cannot be facts about the listing.
// Those take the `core::apply_egress_refusal` path: Indeterminate/NoTunnel,
// `consecutive_failures` untouched, and a retry tomorrow rather than in 45
// days. Letting our own outage accumulate in the streak would read, weeks
// later, as a broker that keeps refusing us, and could argue a case toward
// Abandoned on the strength of an expired VPN subscription.
//
// False for everything else, INCLUDING `Timeout`, `Tls` and `Connect`. Those
// may well be our network in the end, but they are indistinguishable here from
// a broker that is slow, blocking, or misconfigured, and a caller that cannot
// tell the difference must not silently forgive the broker. `BadUrl` is false
// for a different reason: it is nobody's outage, it is a case whose url will
// not parse, and retrying it tomorrow would be a lie about what needs fixing.
//
// Pure, and exercised headless: this is a fact about the vocabulary, decided
// in the file that defines it rather than in whichever window is calling.
bool fetch_error_is_ours(FetchError e);

// ── The result ───────────────────────────────────────────────────────────────
// Default-constructed this is a failure, like every other struct in this
// codebase that a lower layer fills in: a result nobody wrote to must not read
// as a successful fetch of an empty page, because an empty page is very close
// to a removal.
struct FetchResult {
    FetchError error = FetchError::NotBuilt;

    // Meaningful only when `error == Refused`. Carried so the caller can show
    // the user which of the eighteen refusals happened without re-running the
    // check, and so `apply_egress_refusal` has its reason to hand.
    core::Verdict verdict = core::Verdict::Pass;

    int         status = 0;   // the HTTP status. 0 when nothing was received.
    std::string body;         // PII: the broker's page about the user

    // Round trip in milliseconds, for a settings page that wants to say the
    // tunnel is slow. Not evidence of anything.
    long elapsed_ms = 0;
};

inline bool fetch_ok(const FetchResult& r) { return r.error == FetchError::None; }

// ── Is this a url we will point a socket at ──────────────────────────────────
// http and https only, with a host. Pure, and guarded HERE rather than left to
// libcurl, because libcurl speaks twenty-odd protocols and a `file://` in a
// case row is a local file read triggered by a pasted string. `core::Intake`
// already refuses non-http at paste time; this is the same refusal at the other
// end of the pipe, on the theory that the check which matters is the one
// nearest the syscall.
bool fetch_url_ok(const std::string& url);

// ── How the handle is tied to the tunnel ─────────────────────────────────────
// Derived from the policy and the observation by `fetch()`. Public because the
// observer needs to build one before an observation exists to derive it from.
struct FetchBinding {
    // The address to bind to -- the same one `core::iface_pick_address` chose
    // and the observer reported, never the interface NAME. Binding by name is
    // `SO_BINDTODEVICE`, which needs CAP_NET_RAW; binding by address needs
    // nothing and is what `egress_check` compares against anyway. Empty here
    // means no bind, which means `fetch_unchecked` refuses.
    std::string bind_address;

    std::string proxy;         // socks5h://host:port, or empty
    std::string dns_servers;   // pinned resolver, or empty
};

FetchBinding fetch_binding_from(const core::EgressPolicy& p,
                                const core::EgressObservation& o);

// ── The call the check makes ─────────────────────────────────────────────────
// Runs `egress_check` first and returns `Refused` with the verdict if it does
// not pass. This is the only entry point anything above `net/` should use: the
// gate is not a thing the caller remembers to do, it is the function.
FetchResult fetch(const FetchRequest& req, const core::EgressPolicy& p,
                  const core::EgressObservation& o, std::int64_t now_s);

// ── The call the observer makes ──────────────────────────────────────────────
// Ungated, because the preflight is what PRODUCES the observation that the gate
// judges, and a gate that required its own evidence would never run once.
//
// It is still bound: `binding.bind_address` empty is a hard `BindFailed` here,
// so even the preflight cannot leave the machine off-tunnel. What it skips is
// the far-end identity check -- which is the whole point, since answering
// "which exit is this" is the request it is about to make.
//
// Nothing above `net/Observer` should call this. If a second caller ever
// appears, the question to ask is what evidence it thinks it is gathering.
FetchResult fetch_unchecked(const FetchRequest& req, const FetchBinding& binding);

}  // namespace delr::net
