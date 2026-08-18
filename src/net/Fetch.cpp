#include "net/Fetch.hpp"

#include <cctype>
#include <cstring>

#ifdef DELR_HAVE_CURL
#include <curl/curl.h>
#endif

namespace delr::net {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

const char* fetch_default_user_agent() {
    return "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0";
}

// ── The url guard ────────────────────────────────────────────────────────────
bool fetch_url_ok(const std::string& url) {
    const std::string u = url;
    if (u.empty() || u.size() > 2048) return false;
    for (unsigned char c : u)
        if (c < 0x21 || c == 0x7f) return false;   // no spaces, no controls

    const std::string l = lower(u);
    std::size_t after = 0;
    if (l.rfind("https://", 0) == 0)     after = 8;
    else if (l.rfind("http://", 0) == 0) after = 7;
    else return false;

    // There has to be a host, and it has to end before the path does.
    const std::string rest = u.substr(after);
    if (rest.empty()) return false;
    const std::size_t end = rest.find_first_of("/?#");
    const std::string authority = (end == std::string::npos) ? rest : rest.substr(0, end);
    if (authority.empty()) return false;

    // Credentials in a listing url are either a paste accident or somebody
    // else's session. Neither belongs on a handle we are about to bind to a
    // tunnel and send out.
    if (authority.find('@') != std::string::npos) return false;

    // A host needs at least one character that could be part of one.
    for (char c : authority)
        if (std::isalnum(static_cast<unsigned char>(c))) return true;
    return false;
}

// ── The error vocabulary ─────────────────────────────────────────────────────
const char* fetch_error_name(FetchError e) {
    switch (e) {
        case FetchError::None:              return "ok";
        case FetchError::NotBuilt:          return "not-built";
        case FetchError::BadUrl:            return "bad-url";
        case FetchError::Refused:           return "refused";
        case FetchError::DnsPinUnavailable: return "dns-pin-unavailable";
        case FetchError::BindFailed:        return "bind-failed";
        case FetchError::ProxyFailed:       return "proxy-failed";
        case FetchError::Resolve:           return "resolve";
        case FetchError::Connect:           return "connect";
        case FetchError::Tls:               return "tls";
        case FetchError::Timeout:           return "timeout";
        case FetchError::TooLarge:          return "too-large";
        case FetchError::Protocol:          return "protocol";
        case FetchError::Other:             return "other";
    }
    return "other";
}

// Written as an explicit list of the OURS cases with everything else falling
// through, rather than the other way round: a new error value added later must
// default to "theirs and therefore evidence-shaped", which is the reading that
// keeps a caller honest until someone decides otherwise. See the header.
bool fetch_error_is_ours(FetchError e) {
    switch (e) {
        case FetchError::Refused:            // the policy said no
        case FetchError::BindFailed:         // the killswitch fired
        case FetchError::ProxyFailed:        // our proxy, not their site
        case FetchError::DnsPinUnavailable:  // our libcurl
        case FetchError::NotBuilt:           // our build
            return true;
        default:
            return false;
    }
}

const char* fetch_error_text(FetchError e) {
    switch (e) {
        case FetchError::None:
            return "The page was fetched.";
        case FetchError::NotBuilt:
            return "This build has no network support compiled in, so checks "
                   "cannot run.";
        case FetchError::BadUrl:
            return "That address is not a web page this app will fetch.";
        case FetchError::Refused:
            return "The check was not sent, because it could not go out through "
                   "the tunnel.";
        case FetchError::DnsPinUnavailable:
            return "This computer's network library cannot send lookups to the "
                   "resolver you named, so they would fall back to the ordinary "
                   "one. Refused. Route lookups through the proxy instead.";
        case FetchError::BindFailed:
            return "The connection could not be tied to the tunnel, so nothing "
                   "was sent. The tunnel has probably dropped.";
        case FetchError::ProxyFailed:
            return "The proxy did not accept the connection.";
        case FetchError::Resolve:
            return "The site's name could not be looked up.";
        case FetchError::Connect:
            return "The site could not be reached.";
        case FetchError::Tls:
            return "The secure connection to the site could not be established.";
        case FetchError::Timeout:
            return "The site took too long to answer.";
        case FetchError::TooLarge:
            return "The page was larger than this app will read, so it was not "
                   "judged. A page read in part is not a page.";
        case FetchError::Protocol:
            return "The site's reply could not be understood.";
        case FetchError::Other:
            return "The page could not be fetched.";
    }
    return "The page could not be fetched.";
}

// ── Binding ──────────────────────────────────────────────────────────────────
FetchBinding fetch_binding_from(const core::EgressPolicy& p,
                                const core::EgressObservation& o) {
    FetchBinding b;
    // The address the observer reported binding to, not the interface name and
    // not the policy's idea of it: this is the one `egress_check` compared, so
    // it is the one that was cleared.
    b.bind_address = o.bound_address;
    if (p.dns == core::DnsMode::Proxied) b.proxy = p.proxy;
    if (p.dns == core::DnsMode::Pinned)  b.dns_servers = p.resolver;
    return b;
}

#ifndef DELR_HAVE_CURL

// ── No libcurl ───────────────────────────────────────────────────────────────
// The app still builds, the selftest still runs, and every fetch says plainly
// that there is nothing under it. Nothing here pretends.
bool        fetch_available()   { return false; }
std::string fetch_backend()     { return {}; }
bool        fetch_can_pin_dns() { return false; }

FetchResult fetch_unchecked(const FetchRequest&, const FetchBinding&) {
    FetchResult r;
    r.error = FetchError::NotBuilt;
    return r;
}

FetchResult fetch(const FetchRequest&, const core::EgressPolicy&,
                  const core::EgressObservation&, std::int64_t) {
    FetchResult r;
    r.error = FetchError::NotBuilt;
    return r;
}

#else

namespace {

struct Sink {
    std::string  body;
    std::size_t  cap = 0;
    bool         over = false;
};

std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* ud) {
    Sink* s = static_cast<Sink*>(ud);
    const std::size_t n = size * nmemb;
    if (s->body.size() + n > s->cap) {
        // Returning short aborts the transfer with CURLE_WRITE_ERROR. The body
        // is dropped rather than kept: see FetchRequest::max_bytes.
        s->over = true;
        return 0;
    }
    s->body.append(ptr, n);
    return n;
}

FetchError from_curl(CURLcode c) {
    switch (c) {
        case CURLE_OK:                       return FetchError::None;
        case CURLE_URL_MALFORMAT:
        case CURLE_UNSUPPORTED_PROTOCOL:     return FetchError::BadUrl;
        case CURLE_INTERFACE_FAILED:         return FetchError::BindFailed;
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_PROXY:                    return FetchError::ProxyFailed;
        case CURLE_COULDNT_RESOLVE_HOST:     return FetchError::Resolve;
        case CURLE_COULDNT_CONNECT:          return FetchError::Connect;
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_ISSUER_ERROR:         return FetchError::Tls;
        case CURLE_OPERATION_TIMEDOUT:       return FetchError::Timeout;
        case CURLE_TOO_MANY_REDIRECTS:
        case CURLE_GOT_NOTHING:
        case CURLE_WEIRD_SERVER_REPLY:
        case CURLE_PARTIAL_FILE:
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:               return FetchError::Protocol;
        default:                             return FetchError::Other;
    }
}

// One global init, once, and never a teardown: curl_global_cleanup is not safe
// to call while any other thread might still be in the library, and a
// desktop app's exit is exactly when that is hardest to know.
void ensure_global_init() {
    static const bool done = [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return true;
    }();
    (void)done;
}

}  // namespace

bool fetch_available() { return true; }

std::string fetch_backend() {
    ensure_global_init();
    const curl_version_info_data* v = curl_version_info(CURLVERSION_NOW);
    if (!v) return {};
    std::string s = "libcurl ";
    s += v->version ? v->version : "?";
    if (v->ssl_version) { s += " ("; s += v->ssl_version; s += ")"; }
    return s;
}

bool fetch_can_pin_dns() {
    ensure_global_init();
    // Asked of the library rather than assumed from the version: this is a
    // build-time choice by whoever packaged libcurl, and the stock Debian and
    // Ubuntu builds answer no. Probing a throwaway handle is the only honest
    // way to find out.
    static const bool can = [] {
        CURL* h = curl_easy_init();
        if (!h) return false;
        const CURLcode rc = curl_easy_setopt(h, CURLOPT_DNS_SERVERS, "127.0.0.1");
        curl_easy_cleanup(h);
        return rc == CURLE_OK;
    }();
    return can;
}

FetchResult fetch_unchecked(const FetchRequest& req, const FetchBinding& binding) {
    FetchResult r;
    r.error = FetchError::Other;

    if (!fetch_url_ok(req.url)) { r.error = FetchError::BadUrl; return r; }

    // No address, no request. There is no unbound path through this file, and
    // this is the line that makes that true rather than customary.
    if (binding.bind_address.empty()) { r.error = FetchError::BindFailed; return r; }

    if (!binding.dns_servers.empty() && !fetch_can_pin_dns()) {
        r.error = FetchError::DnsPinUnavailable;
        return r;
    }

    ensure_global_init();
    CURL* h = curl_easy_init();
    if (!h) { r.error = FetchError::Other; return r; }

    Sink sink;
    sink.cap = req.max_bytes;

    // "host!<addr>" is curl's spelling for bind-to-address as opposed to
    // bind-to-device. Device binding is SO_BINDTODEVICE and wants CAP_NET_RAW;
    // address binding wants nothing, and the address is what egress_check
    // compared against in the first place.
    const std::string iface = "host!" + binding.bind_address;
    curl_easy_setopt(h, CURLOPT_INTERFACE, iface.c_str());

    curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);

    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, req.max_redirects);
    // Twenty-odd protocols are compiled into libcurl and we want two of them.
    // The redirect list is the one that matters: the url guard above cannot see
    // where a 302 points.
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");

    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, req.connect_timeout_s);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, req.timeout_s);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);

    // Verification stays on. There is no option in this app to turn it off and
    // there should never be one: a tool whose whole claim is "the tunnel is
    // real" cannot also shrug at whether the far end is.
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);

    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");   // whatever this build has

    // Cookies live in this handle and die with it. A broker that sets a session
    // cookie across its own redirect works; nothing is written to disk, because
    // a cookie jar is a record of which listings were checked and when, sitting
    // in a file, which is the one thing this app promises not to keep.
    curl_easy_setopt(h, CURLOPT_COOKIEFILE, "");

    const std::string ua = req.user_agent.empty() ? fetch_default_user_agent()
                                                  : req.user_agent;
    curl_easy_setopt(h, CURLOPT_USERAGENT, ua.c_str());

    if (!binding.proxy.empty()) {
        curl_easy_setopt(h, CURLOPT_PROXY, binding.proxy.c_str());
        // Belt and braces: the scheme in the string already says socks5h, and
        // this says it again on the handle so a policy that somehow arrived
        // here without one cannot resolve locally.
        curl_easy_setopt(h, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
    }
    if (!binding.dns_servers.empty())
        curl_easy_setopt(h, CURLOPT_DNS_SERVERS, binding.dns_servers.c_str());

    const CURLcode rc = curl_easy_perform(h);

    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_off_t total_us = 0;
    curl_easy_getinfo(h, CURLINFO_TOTAL_TIME_T, &total_us);
    curl_easy_cleanup(h);

    r.status     = static_cast<int>(status);
    r.elapsed_ms = static_cast<long>(total_us / 1000);

    if (rc == CURLE_WRITE_ERROR && sink.over) {
        r.error = FetchError::TooLarge;
        return r;   // body deliberately not carried
    }
    r.error = from_curl(rc);
    if (r.error == FetchError::None) r.body = sink.body;
    return r;
}

FetchResult fetch(const FetchRequest& req, const core::EgressPolicy& p,
                  const core::EgressObservation& o, std::int64_t now_s) {
    FetchResult r;

    // The gate, and it is the first statement in the function on purpose. There
    // is no argument, no flag and no overload that reaches the socket below
    // without passing this.
    const core::Verdict v = core::egress_check(p, o, now_s);
    if (!core::verdict_clear(v)) {
        r.error   = FetchError::Refused;
        r.verdict = v;
        return r;
    }
    return fetch_unchecked(req, fetch_binding_from(p, o));
}

#endif  // DELR_HAVE_CURL

}  // namespace delr::net
