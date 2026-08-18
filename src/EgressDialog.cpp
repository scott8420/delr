// EgressDialog -- arranges labels around core::Egress and net::Observer.
// No judgments here; see the header.
#include "EgressDialog.hpp"
#include "Log.hpp"

#include "core/Intake.hpp"   // url_normalize, for the two endpoint boxes
#include "net/Fetch.hpp"

#include <gtkmm/label.h>

#include <chrono>
#include <cstdlib>

namespace delr {
namespace {

// The modes a person may choose. `System` is absent on purpose and this is the
// list that enforces it: `core/Egress` refuses it with no flag, so offering it
// here would be offering a switch that does nothing but produce a refusal.
// `Unset` is present because a policy that has never been configured IS unset,
// and a dropdown that could not display the current value would be lying about
// the state of the file.
// The modes a person may choose. `System` is absent on purpose and this is the
// list that enforces it: `core/Egress` refuses it with no flag, so offering it
// here would be offering a switch that does nothing but produce a refusal.
// `SystemVerified` is present and is a DIFFERENT value -- the same resolver
// with the canary made load-bearing -- which is why the label says "checked"
// rather than naming the resolver.
//
// ORDERED STRONGEST FIRST, below "Not set yet". The list is the only place a
// user reads the modes side by side, and a list that led with the weakest one
// would be recommending it.
const std::pair<core::DnsMode, const char*> kModes[] = {
    {core::DnsMode::Unset,          "Not set yet"},
    {core::DnsMode::Proxied,        "Through a SOCKS5 proxy"},
    {core::DnsMode::Pinned,         "To a named resolver"},
    {core::DnsMode::SystemVerified, "This computer's own, checked every time"},
};

// One sentence per mode, and they are meant to be read against each other --
// the whole judgment a person makes here is which of these they can live with.
// The weaker one is labelled weaker in words rather than by omission or by a
// warning icon: the mode most people can actually use is the last one, and
// scaring them off it would send them back to no tunnel at all.
const char* mode_note(core::DnsMode m) {
    switch (m) {
        case core::DnsMode::Unset:
            return "Until this is set, nothing is allowed out. Using this "
                   "computer's own resolver without checking it is not on the "
                   "list: it leaks every site you check and there is no setting "
                   "for it.";
        case core::DnsMode::Proxied:
            return "Strongest. The name never leaves this computer — the proxy "
                   "looks it up at the far end, so there is nothing to leak "
                   "rather than a leak that gets caught.";
        case core::DnsMode::Pinned:
            return "Strong. Lookups go to one resolver you name, reached inside "
                   "the tunnel, and a different one answering is a refusal.";
        case core::DnsMode::SystemVerified:
            return "Weaker than the two above, and usually the only one an "
                   "ordinary VPN account can meet. Lookups use whatever this "
                   "computer normally uses — which, with the tunnel up, is your "
                   "VPN provider's. delr cannot stop one escaping; what it does "
                   "is notice, by checking every run whether the answer came "
                   "from a resolver it has seen answer you WITHOUT the tunnel. "
                   "That needs recording below, and it is only as good as the "
                   "resolvers it has seen.";
        case core::DnsMode::System:
            break;   // never offered; see the list above
    }
    return "";
}

std::int64_t now_monotonic_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const char* yn(bool b) { return b ? "yes" : "no"; }

// ── the device list, as one line of text ─────────────────────────────────────
// Adjacent on purpose, and the rule is the usual one: what is written must read
// back identically. Split drops empties and trims, so "a, ,b," and "a,b" are
// the same list, and join never emits a trailing separator -- otherwise a save
// followed by a load would grow an empty entry every round trip.
std::vector<std::string> devs_split(const std::string& text) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t c = text.find(',', pos);
        std::string part = text.substr(pos, c == std::string::npos ? std::string::npos
                                                                   : c - pos);
        pos = (c == std::string::npos) ? text.size() + 1 : c + 1;

        const std::size_t b = part.find_first_not_of(" \t");
        if (b == std::string::npos) continue;          // blank between commas
        const std::size_t e = part.find_last_not_of(" \t");
        out.push_back(part.substr(b, e - b + 1));
    }
    return out;
}

std::string devs_join(const std::vector<std::string>& devs) {
    std::string s;
    for (const auto& d : devs) {
        if (d.empty()) continue;
        if (!s.empty()) s += ", ";
        s += d;
    }
    return s;
}

}  // namespace

EgressDialog::EgressDialog()
    : m_frame("egress.frame", Gtk::Orientation::VERTICAL, 0),
      m_root("egress.root", Gtk::Orientation::VERTICAL, 10),
      m_iface_row("egress.iface.row", Gtk::Orientation::HORIZONTAL, 8),
      m_dns_row("egress.dns.row", Gtk::Orientation::HORIZONTAL, 8),
      m_proxy_row("egress.proxy.row", Gtk::Orientation::HORIZONTAL, 8),
      m_resolver_row("egress.resolver.row", Gtk::Orientation::HORIZONTAL, 8),
      m_ttl_row("egress.ttl.row", Gtk::Orientation::HORIZONTAL, 8),
      m_exits_head("egress.exits.head", Gtk::Orientation::HORIZONTAL, 8),
      m_baseline_row("egress.baseline.row", Gtk::Orientation::HORIZONTAL, 8),
      m_preflight_row("egress.preflight.row", Gtk::Orientation::HORIZONTAL, 8),
      m_echo_row("egress.echo.row", Gtk::Orientation::HORIZONTAL, 8),
      m_canary_row("egress.canary.row", Gtk::Orientation::HORIZONTAL, 8),
      m_devs_row("egress.devs.row", Gtk::Orientation::HORIZONTAL, 8),
      m_buttons("egress.buttons", Gtk::Orientation::HORIZONTAL, 8),
      m_intro("egress.intro"),
      m_iface_label("egress.iface.label"),
      m_iface_live("egress.iface.live"),
      m_devs_label("egress.devs.label"),
      m_devs_note("egress.devs.note"),
      m_dns_label("egress.dns.label"),
      m_dns_note("egress.dns.note"),
      m_proxy_label("egress.proxy.label"),
      m_proxy_state("egress.proxy.state"),
      m_resolver_label("egress.resolver.label"),
      m_resolver_note("egress.resolver.note"),
      m_ttl_label("egress.ttl.label"),
      m_exits_label("egress.exits.label"),
      m_baseline_label("egress.baseline.label"),
      m_baseline_state("egress.baseline.state"),
      m_resolver_state("egress.baseline.resolvers.state"),
      m_endpoints_label("egress.endpoints.label"),
      m_echo_label("egress.echo.label"),
      m_canary_label("egress.canary.label"),
      m_endpoints_note("egress.endpoints.note"),
      m_report("egress.report"),
      m_verdict("egress.verdict"),
      m_problems("egress.problems"),
      m_step1("egress.step1"),
      m_step2("egress.step2"),
      m_step3("egress.step3"),
      m_baseline_say("egress.baseline.say"),
      m_iface_say("egress.iface.say"),
      m_exits_say("egress.exits.say"),

      m_iface("egress.iface"),
      m_devs("egress.devs"),
      m_proxy("egress.proxy"),
      m_resolver("egress.resolver"),
      m_ttl("egress.ttl"),
      m_baseline_iface("egress.baseline.iface"),
      m_echo_url("egress.echo.url"),
      m_canary_url("egress.canary.url"),
      m_dns("egress.dns"),
      m_exits("egress.exits"),
      m_exits_scroll("egress.exits.scroll"),
      m_scroll("egress.scroll"),
      m_trust("egress.trust", "Trust this exit"),
      m_record("egress.record", "Record"),
      m_forget_baseline("egress.baseline.forget", "Forget"),
      m_forget_resolvers("egress.baseline.resolvers.forget", "Forget lookups"),
      m_preflight("egress.preflight", "Run preflight"),
      m_close("egress.close", "Close"),
      m_save("egress.save", "Save"),
      m_detect_naked("egress.baseline.detect", "Find it"),
      m_detect_tunnel("egress.iface.detect", "Find it") {
    set_name("egress");
    build();
    bind();
}

EgressDialog::~EgressDialog() {
    // The worker touches no widget, but it does write members of this object,
    // so it must not outlive it.
    if (m_worker.joinable()) m_worker.join();
}

void EgressDialog::build() {
    set_title("Tunnel and privacy");
    // A real height rather than -1. With the body in a scroller, -1 asks for
    // the natural height of everything inside it -- which is the too-tall
    // window this scroller exists to fix. 720 fits a 768-high screen with room
    // for a panel; anything taller and the scroller takes over.
    set_default_size(620, 720);
    set_modal(true);
    set_hide_on_close(true);

    m_root.set_margin(16);

    m_intro.set_text(
        "Checks go out through a tunnel or they do not go out at all. A tunnel "
        "on its own is not enough: name lookups have to go through it too, or "
        "the list of sites you check leaks to whoever answers them while every "
        "page you fetch looks perfectly private.");
    m_intro.set_wrap(true);
    m_intro.set_xalign(0.0f);
    m_intro.add_css_class("dim-label");

    // ── the three steps, as headings ─────────────────────────────────────────
    // Numbers rather than prose, and the VPN's state in the heading rather than
    // buried in a note, because the single most expensive confusion in s10 was
    // about which way round the tunnel was supposed to be for the step being
    // attempted. A person who reads nothing else in this window reads these.
    const auto head = [](widgets::Label& l, const char* text) {
        l.set_text(text);
        l.set_xalign(0.0f);
        l.set_wrap(true);
        l.add_css_class("heading");
        l.set_margin_top(6);
    };
    head(m_step1, "1. Your ordinary connection — with the VPN OFF");
    head(m_step2, "2. Your tunnel — with the VPN ON");
    head(m_step3, "3. Check it");

    // Each step's own sink. A sentence produced by a button belongs beside that
    // button; see the header for the wall that taught us so.
    for (widgets::Label* l : {&m_baseline_say, &m_iface_say, &m_exits_say}) {
        l->set_xalign(0.0f);
        l->set_wrap(true);
    }

    // ── the tunnel ───────────────────────────────────────────────────────────
    // An entry rather than a dropdown of live interfaces, deliberately: a
    // tunnel that is DOWN does not appear in `iface_list()`, and a dropdown
    // would silently drop the user's configuration every time they opened this
    // window with the VPN off. The live list goes underneath as a hint, which
    // solves the real problem ("wg0" is a guess) without the trap.
    m_iface_label.set_text("Tunnel");
    m_iface.set_placeholder_text("wg0");
    m_iface.set_hexpand(true);
    m_iface_row.append(m_iface_label);
    m_iface_row.append(m_iface);
    // Proposes; does not commit. The box stays editable and the name it writes
    // is one the kernel just used, not one this file guessed at.
    m_iface_row.append(m_detect_tunnel);
    m_iface_live.set_xalign(0.0f);
    m_iface_live.set_wrap(true);
    m_iface_live.add_css_class("dim-label");

    // ── the tunnel's other devices ───────────────────────────────────────────
    // Separate from the box above because they answer separate questions: that
    // one is what a socket binds to and has exactly one answer, this one is
    // what counts as "inside the tunnel" when reading the routing table and
    // legitimately has several. See `EgressPolicy::tunnel_devs`.
    m_devs_label.set_text("Also this tunnel");
    m_devs.set_placeholder_text("wg0-v6, another_device");
    m_devs.set_hexpand(true);
    m_devs_row.append(m_devs_label);
    m_devs_row.append(m_devs);
    m_devs_note.set_text(
        "Optional, comma separated. Some providers split one tunnel across two "
        "devices — commonly a second one carrying IPv6. Without its name here, "
        "delr reads that device's default route as traffic going around the "
        "tunnel and refuses to check anything. Only add devices you know are "
        "part of your tunnel: a name listed here stops a leak being reported "
        "on it.");
    m_devs_note.set_xalign(0.0f);
    m_devs_note.set_wrap(true);
    m_devs_note.add_css_class("dim-label");

    // ── how names get resolved ───────────────────────────────────────────────
    m_dns_label.set_text("Name lookups");
    m_dns_names = Gtk::StringList::create({});
    for (const auto& m : kModes) m_dns_names->append(m.second);
    m_dns.set_model(m_dns_names);
    m_dns.set_hexpand(true);
    m_dns_row.append(m_dns_label);
    m_dns_row.append(m_dns);
    m_dns_note.set_xalign(0.0f);
    m_dns_note.set_wrap(true);
    m_dns_note.add_css_class("dim-label");

    m_proxy_label.set_text("Proxy");
    m_proxy.set_placeholder_text("socks5h://127.0.0.1:1080");
    m_proxy.set_hexpand(true);
    m_proxy.set_input_purpose(Gtk::InputPurpose::URL);
    m_proxy_row.append(m_proxy_label);
    m_proxy_row.append(m_proxy);
    m_proxy_state.set_xalign(0.0f);
    m_proxy_state.set_wrap(true);

    m_resolver_label.set_text("Resolver");
    m_resolver.set_placeholder_text("an address reachable inside the tunnel");
    m_resolver.set_hexpand(true);
    m_resolver_row.append(m_resolver_label);
    m_resolver_row.append(m_resolver);
    m_resolver_note.set_xalign(0.0f);
    m_resolver_note.set_wrap(true);
    m_resolver_note.add_css_class("dim-label");

    // ── how long a pass is good for ──────────────────────────────────────────
    m_ttl_label.set_text("A preflight pass is good for (seconds)");
    m_ttl.set_max_width_chars(8);
    m_ttl.set_input_purpose(Gtk::InputPurpose::DIGITS);
    m_ttl_row.append(m_ttl_label);
    m_ttl_row.append(m_ttl);

    // ── the exits we accept ──────────────────────────────────────────────────
    m_exits_label.set_text("Trusted exits");
    m_exits_label.set_xalign(0.0f);
    m_exits_label.set_hexpand(true);
    m_trust.set_sensitive(false);
    m_exits_head.append(m_exits_label);
    m_exits_head.append(m_trust);
    m_exits.set_selection_mode(Gtk::SelectionMode::NONE);
    m_exits_scroll.set_child(m_exits);
    m_exits_scroll.set_min_content_height(90);

    // ── the baseline ─────────────────────────────────────────────────────────
    // The address is never shown -- see the header. The word "recorded" is the
    // whole of what a person needs from it.
    // One press records BOTH halves, because they are one fact about one
    // moment: what this computer looks like, and who answers its lookups, with
    // the tunnel off. A second button would be a second thing to forget, at a
    // second moment when the tunnel might be back up.
    m_baseline_label.set_text("What this computer looks like without the tunnel");
    m_baseline_label.set_xalign(0.0f);
    m_baseline_state.set_xalign(0.0f);
    m_baseline_state.set_wrap(true);
    m_resolver_state.set_xalign(0.0f);
    m_resolver_state.set_wrap(true);
    m_baseline_iface.set_placeholder_text("eth0");
    m_baseline_iface.set_max_width_chars(10);
    m_baseline_row.append(m_detect_naked);
    m_baseline_row.append(m_baseline_iface);
    m_baseline_row.append(m_record);
    m_baseline_row.append(m_forget_baseline);
    m_baseline_row.append(m_forget_resolvers);
    m_baseline_row.set_halign(Gtk::Align::START);

    // ── the two endpoints ────────────────────────────────────────────────────
    // Literals until s9. They became settings the moment the canary stopped
    // corroborating a proxy and started BEING the guarantee: a third party that
    // goes away used to be an annoyance and is now every check refusing, with
    // no fix short of a rebuild.
    m_endpoints_label.set_text("Where the preflight asks");
    m_endpoints_label.set_xalign(0.0f);
    m_echo_label.set_text("Address check");
    m_echo_url.set_hexpand(true);
    m_echo_url.set_input_purpose(Gtk::InputPurpose::URL);
    m_echo_row.append(m_echo_label);
    m_echo_row.append(m_echo_url);
    m_canary_label.set_text("Lookup check");
    m_canary_url.set_hexpand(true);
    m_canary_url.set_input_purpose(Gtk::InputPurpose::URL);
    m_canary_row.append(m_canary_label);
    m_canary_row.append(m_canary_url);
    m_endpoints_note.set_xalign(0.0f);
    m_endpoints_note.set_wrap(true);
    m_endpoints_note.add_css_class("dim-label");
    m_endpoints_note.set_text(
        "Neither is trusted with anything: the first is checked against what "
        "you recorded above, and the second is only ever used to refuse. A "
        "hostile one can stop checks running; it cannot let one through. "
        "Emptying the lookup check switches the leak test off, which refuses "
        "everything.");

    // ── the preflight ────────────────────────────────────────────────────────
    m_report.set_xalign(0.0f);
    m_report.set_wrap(true);
    m_report.add_css_class("monospace");
    m_verdict.set_xalign(0.0f);
    m_verdict.set_wrap(true);
    m_preflight_row.append(m_preflight);
    m_preflight_row.set_halign(Gtk::Align::START);

    m_problems.set_xalign(0.0f);
    m_problems.set_wrap(true);

    m_save.add_css_class("suggested-action");
    m_buttons.set_halign(Gtk::Align::END);
    m_buttons.append(m_close);
    m_buttons.append(m_save);

    // ── the order of the window IS the order of the work ─────────────────────
    // Until s11 this read as a pile of fields in the order they were built, and
    // a first run through it needed a chat window open beside the app. The
    // sequence below is the real one: record what you look like without the
    // tunnel (which can only be done with the VPN off, so it is genuinely
    // first), name the tunnel (which needs the VPN on), then check it. Settings
    // sit underneath the path rather than through the middle of it.
    m_root.append(m_intro);

    m_root.append(m_step1);
    m_root.append(m_baseline_label);
    m_root.append(m_baseline_state);
    m_root.append(m_resolver_state);
    m_root.append(m_baseline_row);
    m_root.append(m_baseline_say);

    m_root.append(m_step2);
    m_root.append(m_iface_row);
    m_root.append(m_iface_live);
    m_root.append(m_devs_row);
    m_root.append(m_devs_note);
    m_root.append(m_iface_say);

    m_root.append(m_step3);
    m_root.append(m_preflight_row);
    m_root.append(m_report);
    m_root.append(m_verdict);

    // Everything below here is settings rather than steps.
    m_root.append(m_dns_row);
    m_root.append(m_dns_note);
    m_root.append(m_proxy_row);
    m_root.append(m_proxy_state);
    m_root.append(m_resolver_row);
    m_root.append(m_resolver_note);
    m_root.append(m_ttl_row);
    m_root.append(m_exits_head);
    m_root.append(m_exits_scroll);
    m_root.append(m_exits_say);
    m_root.append(m_endpoints_label);
    m_root.append(m_echo_row);
    m_root.append(m_canary_row);
    m_root.append(m_endpoints_note);
    m_root.append(m_problems);

    // ── the scroller ─────────────────────────────────────────────────────────
    // s9 appended six widgets and two entry boxes to a plain vertical Box that
    // nothing constrained, and the window grew taller than the screen -- Record
    // and Save both fell off the bottom, unreachable. Found the first time
    // anyone opened it, which is the whole argument for the visual channel.
    //
    // The BUTTONS STAY OUT of the scroller. A settings window whose Save can
    // scroll out of sight is the same bug in miniature, and this window has a
    // second reason: it is the one surface where the difference between an
    // edited policy and a SAVED one decides whether a check refuses. Save is
    // always on screen.
    //
    // Vertical only. Horizontal scrolling would let a long label push the width
    // out instead of wrapping, and every long string in here already wraps.
    m_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_scroll.set_child(m_root);
    m_scroll.set_vexpand(true);

    // `m_exits_scroll` is a scroller inside a scroller, which normally captures
    // the wheel and traps the reader in a 90px box. It has a min content height
    // and no natural-height propagation, so it keeps its own size rather than
    // fighting the outer one for space.
    m_exits_scroll.set_propagate_natural_height(false);

    m_buttons.set_margin(16);
    m_buttons.set_margin_top(0);

    m_frame.append(m_scroll);
    m_frame.append(m_buttons);
    set_child(m_frame);
}

void EgressDialog::bind() {
    m_iface.signal_changed().connect(sigc::mem_fun(*this, &EgressDialog::refresh));
    m_proxy.signal_changed().connect(sigc::mem_fun(*this, &EgressDialog::refresh));
    m_resolver.signal_changed().connect(sigc::mem_fun(*this, &EgressDialog::refresh));
    m_devs.signal_changed().connect(sigc::mem_fun(*this, &EgressDialog::refresh));
    m_ttl.signal_changed().connect(sigc::mem_fun(*this, &EgressDialog::refresh));
    m_echo_url.signal_changed().connect(sigc::mem_fun(*this, &EgressDialog::refresh));
    m_canary_url.signal_changed().connect(sigc::mem_fun(*this, &EgressDialog::refresh));
    // The Record button is gated on this box having something in it.
    m_baseline_iface.signal_changed().connect(sigc::mem_fun(*this, &EgressDialog::refresh));
    m_dns.property_selected().signal_changed().connect(
        sigc::mem_fun(*this, &EgressDialog::refresh));

    m_preflight.signal_clicked().connect(
        sigc::mem_fun(*this, &EgressDialog::on_run_preflight));
    m_record.signal_clicked().connect(
        sigc::mem_fun(*this, &EgressDialog::on_record_baseline));
    m_forget_baseline.signal_clicked().connect(
        sigc::mem_fun(*this, &EgressDialog::on_forget_baseline));
    m_forget_resolvers.signal_clicked().connect(
        sigc::mem_fun(*this, &EgressDialog::on_forget_resolvers));
    m_trust.signal_clicked().connect(
        sigc::mem_fun(*this, &EgressDialog::on_trust_exit));
    m_detect_naked.signal_clicked().connect(
        sigc::mem_fun(*this, &EgressDialog::on_detect_naked));
    m_detect_tunnel.signal_clicked().connect(
        sigc::mem_fun(*this, &EgressDialog::on_detect_tunnel));

    m_close.signal_clicked().connect([this] { set_visible(false); });
    m_save.signal_clicked().connect(sigc::mem_fun(*this, &EgressDialog::on_save));
    signal_hide().connect(sigc::mem_fun(*this, &EgressDialog::on_hidden));

    m_done.connect(sigc::mem_fun(*this, &EgressDialog::on_worker_done));
}

// ── open / close ─────────────────────────────────────────────────────────────

void EgressDialog::open(Gtk::Window& parent, const core::EgressPolicy& p) {
    m_policy = p;
    m_trustable = false;
    m_report.set_text("");
    m_verdict.set_text("");
    m_baseline_say.set_text("");
    m_iface_say.set_text("");
    m_exits_say.set_text("");
    plant(m_policy);
    set_transient_for(parent);
    present();
    refresh();
}

void EgressDialog::on_hidden() {
    // The working copy holds `naked_exit`. A hidden window sitting on the
    // user's home address is the same failure as a log file sitting on it.
    m_policy = core::EgressPolicy{};
    m_job_obs = core::EgressObservation{};
    m_job_baseline = net::BaselineResult{};
    m_job_policy = core::EgressPolicy{};
    m_trustable = false;
    m_setting = true;
    m_iface.set_text("");
    m_devs.set_text("");
    m_proxy.set_text("");
    m_resolver.set_text("");
    m_ttl.set_text("");
    m_baseline_iface.set_text("");
    m_echo_url.set_text("");
    m_canary_url.set_text("");
    m_report.set_text("");
    m_verdict.set_text("");
    m_baseline_say.set_text("");
    m_iface_say.set_text("");
    m_exits_say.set_text("");
    m_setting = false;
    repaint_exits();
}

// ── widgets <-> policy ───────────────────────────────────────────────────────

core::DnsMode EgressDialog::chosen_mode() const {
    const auto i = m_dns.get_selected();
    if (i == GTK_INVALID_LIST_POSITION || i >= std::size(kModes))
        return core::DnsMode::Unset;
    return kModes[i].first;
}

void EgressDialog::plant(const core::EgressPolicy& p) {
    m_setting = true;
    m_iface.set_text(p.interface_name);
    m_devs.set_text(devs_join(p.tunnel_devs));
    m_proxy.set_text(p.proxy);
    m_resolver.set_text(p.resolver);
    m_ttl.set_text(std::to_string(p.preflight_ttl_s));
    m_echo_url.set_text(p.echo_url);
    m_canary_url.set_text(p.canary_url);
    // The box a first run had to guess at comes back filled on every run after,
    // because a successful Record wrote down what it was recorded on. Empty on
    // a genuinely first run, which is when the Find it button beside it earns
    // its place.
    m_baseline_iface.set_text(p.naked_device);

    // A policy loaded off a hand-edited file can say `system`, which is not in
    // the list. It shows as "Not set yet" and the problems line below says
    // plainly what was in the file -- rather than the dropdown silently
    // agreeing with something the app refuses.
    guint sel = 0;
    for (guint i = 0; i < std::size(kModes); ++i)
        if (kModes[i].first == p.dns) sel = i;
    m_dns.set_selected(sel);
    m_setting = false;
    repaint_exits();
}

core::EgressPolicy EgressDialog::harvest() const {
    core::EgressPolicy p = m_policy;   // carries naked_exit and the exit list
    p.interface_name = m_iface.get_text();
    p.tunnel_devs    = devs_split(m_devs.get_text());
    p.dns            = chosen_mode();
    p.proxy          = m_proxy.get_text();
    p.resolver       = m_resolver.get_text();

    // Normalised through the same function that guards a pasted listing, so
    // `example.com/x` becomes a url we would actually fetch rather than a
    // validation error the user has to decode. An entry `url_normalize`
    // refuses comes back empty and keeps the text the user typed, so the
    // validator can name it instead of the field silently emptying itself.
    const auto keep_url = [](const std::string& typed) {
        const std::string t = core::url_normalize(typed);
        return t.empty() ? typed : t;
    };
    p.echo_url       = keep_url(m_echo_url.get_text());
    p.canary_url     = keep_url(m_canary_url.get_text());

    const std::string t = m_ttl.get_text();
    char* end = nullptr;
    const long long v = std::strtoll(t.c_str(), &end, 10);
    // Anything unreadable keeps the value the policy already had rather than
    // becoming zero -- a zero ttl means every preflight is stale on arrival,
    // and a typo should not silently switch the app off.
    if (end && *end == '\0' && v > 0) p.preflight_ttl_s = v;
    return p;
}

void EgressDialog::repaint_exits() {
    while (auto* row = m_exits.get_row_at_index(0)) m_exits.remove(*row);
    if (m_policy.accepted_exits.empty()) {
        auto* row = Gtk::make_managed<Gtk::Label>(
            "None yet. Turn the tunnel on and run a preflight — the exit it "
            "finds can be trusted from here.");
        row->set_xalign(0.0f);
        row->set_wrap(true);
        row->set_margin(6);
        row->add_css_class("dim-label");
        m_exits.append(*row);
        return;
    }
    for (const auto& a : m_policy.accepted_exits) {
        // Transient per-row content: named for the Inspector, not registered
        // (the `unregistered_t` primitive).
        auto* row = Gtk::make_managed<widgets::Box>(
            widgets::unregistered, "egress.exit.row", Gtk::Orientation::HORIZONTAL, 8);
        auto* lbl = Gtk::make_managed<Gtk::Label>(a);
        lbl->set_xalign(0.0f);
        lbl->set_hexpand(true);
        lbl->set_margin(6);
        auto* del = Gtk::make_managed<Gtk::Button>("Forget");
        del->set_margin(4);
        del->signal_clicked().connect([this, a] { on_forget_exit(a); });
        row->append(*lbl);
        row->append(*del);
        m_exits.append(*row);
    }
}

// ── the derived labels ───────────────────────────────────────────────────────

void EgressDialog::refresh() {
    if (m_setting) return;

    const core::EgressPolicy p = harvest();
    const core::DnsMode mode = p.dns;

    // Which interfaces exist right now. Names only; addresses are not this
    // label's business.
    std::string live = "On this computer now:";
    const auto names = net::iface_list();
    for (std::size_t i = 0; i < names.size(); ++i)
        live += (i ? ", " : " ") + names[i];
    if (names.empty()) live = "No network connections found.";
    // And whether the one that is typed is any good, which is the question the
    // user actually has.
    const std::string want = p.interface_name;
    if (!want.empty()) {
        const net::IfaceReading r = net::iface_read(want);
        live += "   —   " + want + ": present " + yn(r.present) + ", up " +
                yn(r.up) + ", " + std::to_string(r.addresses.size()) +
                " address(es)";
    }
    m_iface_live.set_text(live);

    // The proxy. `proxy_url_ok` is the judge, here as in the validator.
    const bool proxy_needed = (mode == core::DnsMode::Proxied);
    m_proxy.set_sensitive(proxy_needed);
    if (!proxy_needed) {
        m_proxy_state.set_text("");
    } else if (p.proxy.empty()) {
        m_proxy_state.set_text(
            "A tunnel on its own does not give you one. A provider's SOCKS5 "
            "endpoint reached inside the tunnel is the usual answer.");
        m_proxy_state.add_css_class("dim-label");
    } else if (core::proxy_url_ok(p.proxy)) {
        m_proxy_state.remove_css_class("dim-label");
        m_proxy_state.set_text("Good — names are resolved at the far end.");
    } else {
        m_proxy_state.remove_css_class("dim-label");
        m_proxy_state.set_text(
            "Not usable. It has to be socks5h://host:port — with the h. "
            "Without it the name is looked up on this computer and sent "
            "onward, which leaks every site you check while the pages "
            "themselves go out clean.");
    }

    // Pinning a resolver only works if the library underneath can do it, and on
    // a stock Debian/Ubuntu libcurl it cannot. Asked at RUNTIME rather than
    // assumed, and said out loud rather than failing later as a refusal nobody
    // can account for.
    const bool pinned = (mode == core::DnsMode::Pinned);
    const bool can_pin = net::fetch_can_pin_dns();
    m_resolver.set_sensitive(pinned && can_pin);
    if (!pinned) {
        m_resolver_note.set_text("");
    } else if (!can_pin) {
        m_resolver_note.set_text(
            "This computer's network library cannot send lookups to a named "
            "resolver, so choosing one here would not take effect. Checks will "
            "refuse rather than quietly using this computer's own resolver. "
            "Route lookups through a proxy instead.");
    } else {
        m_resolver_note.set_text(
            "Must be reachable inside the tunnel — a resolver on the local "
            "network is this computer's own resolver by another name.");
    }

    // One sentence per mode, written to be read against each other. The weaker
    // mode says so in words -- see `mode_note`.
    m_dns_note.set_text(mode_note(mode));

    // ── the baseline, as words and a count, never as an address ──────────────
    m_baseline_state.set_text(
        m_policy.naked_exit.empty()
            ? "Your own address: not recorded. Turn the tunnel OFF, name your "
              "ordinary connection, and press Record. Without it, delr cannot "
              "tell a working tunnel from a dead one, and will refuse to check "
              "anything."
            : "Your own address: recorded. delr can tell it from a tunnel's, "
              "which is what makes a dead tunnel a refusal instead of a leak.");

    // The COUNT is shown and the addresses are not. A count is not an address,
    // and it is the only honest way to say how strong this mode currently is:
    // the check recognises a leak by recognising the resolver, so one sample is
    // one resolver's worth of protection.
    const std::size_t seen = m_policy.naked_resolvers.size();
    if (seen == 0) {
        m_resolver_state.set_text(
            "Lookups without the tunnel: not recorded. Needed only for the "
            "checked-every-time mode, which refuses without it — there would be "
            "nothing to recognise a leaked lookup against.");
    } else {
        m_resolver_state.set_text(
            "Lookups without the tunnel: " + std::to_string(seen) +
            (seen == 1 ? " resolver seen." : " resolvers seen.") +
            " A lookup answered by any of them is treated as having escaped the "
            "tunnel. Providers often answer from several, so pressing Record "
            "again on another day — tunnel off — makes this stronger.");
    }

    m_forget_baseline.set_sensitive(!m_policy.naked_exit.empty() && !busy());
    m_forget_resolvers.set_sensitive(seen > 0 && !busy());
    m_record.set_sensitive(!busy() && !m_baseline_iface.get_text().empty());
    m_preflight.set_sensitive(!busy());
    m_detect_naked.set_sensitive(!busy());
    m_detect_tunnel.set_sensitive(!busy());
    m_trust.set_sensitive(m_trustable && !busy());

    // And the policy's own verdict on itself, which is the validator's job and
    // not this file's.
    const auto problems = core::egress_policy_validate(p);
    if (problems.empty()) {
        m_problems.set_text("");
    } else {
        std::string s;
        for (const auto& x : problems) s += (s.empty() ? "" : "\n") + x;
        m_problems.set_text(s);
    }
    m_save.set_sensitive(!busy());
}

// ── the worker slot ──────────────────────────────────────────────────────────

void EgressDialog::start(Job j) {
    if (busy()) return;
    if (m_worker.joinable()) m_worker.join();   // the previous, already finished

    m_job        = j;
    m_job_policy = harvest();
    m_job_iface  = m_baseline_iface.get_text();
    m_job_obs    = core::EgressObservation{};
    m_job_detail = net::Observation{};
    m_job_baseline = net::BaselineResult{};

    refresh();   // the buttons go insensitive here, on the main thread

    // Copies only. The worker reads `m_job_policy` and `m_job_iface`, which the
    // main thread does not touch until `on_worker_done`, and it touches no
    // widget at all.
    m_worker = std::thread([this] {
        const std::int64_t now = now_monotonic_s();
        if (m_job == Job::Preflight) {
            m_job_obs = net::preflight(m_job_policy, net::observer_config(m_job_policy), now,
                                       &m_job_detail);
            m_job_verdict = core::egress_check(m_job_policy, m_job_obs, now);
        } else {
            m_job_baseline = net::baseline_read(m_job_iface, net::observer_config(m_job_policy));
        }
        m_done.emit();
    });
}

void EgressDialog::on_run_preflight() {
    m_report.set_text("Running…");
    m_verdict.set_text("");
    m_trustable = false;
    start(Job::Preflight);
}

void EgressDialog::on_record_baseline() {
    // Recording the baseline through the tunnel would store the TUNNEL's exit
    // as this machine's own address, and `ExitNaked` would then fire on a
    // perfectly good tunnel forever. Refused here because it is a fact about
    // what the user typed into two boxes, which is this file's business.
    if (!m_iface.get_text().empty() &&
        m_baseline_iface.get_text() == m_iface.get_text()) {
        // Beside the button, not at the bottom of the window. In s10 this exact
        // sentence fired correctly and rendered a screen below the press that
        // caused it, so Record looked broken -- which is the whole reason this
        // window now has a sink per step.
        m_baseline_say.set_text(
            "That is the tunnel. Name the connection you use with the tunnel "
            "OFF — the whole point of this number is that it is what you look "
            "like without one.");
        return;
    }
    m_baseline_say.set_text("Asking, over your ordinary connection…");
    start(Job::Baseline);
}

void EgressDialog::on_worker_done() {
    const Job finished = m_job;
    m_job = Job::None;
    if (m_worker.joinable()) m_worker.join();

    auto lg = log::get(log::Area::App);

    if (finished == Job::Preflight) {
        const core::ProbeReadings& r = m_job_detail.readings;

        // The same discipline as `--netcheck`: yes/no, counts, named states.
        // Not one address, not a prefix length. This is the panel somebody
        // screenshots when asking for help.
        std::string s;
        s += "interface present  " + std::string(yn(r.interface_present)) + "\n";
        s += "interface up       " + std::string(yn(r.interface_up)) + "\n";
        s += "addresses found    " + std::to_string(r.interface_addresses.size()) + "\n";
        s += "bound              " + std::string(yn(r.bound)) + "\n";
        s += "echo ran           " + std::string(yn(r.echo_ran)) + "\n";
        s += "canary ran         " + std::string(yn(r.canary_ran)) + "\n";
        s += "exit readable      " + std::string(yn(!m_job_obs.observed_exit.empty())) + "\n";
        s += "v6 off-tunnel      " + std::string(yn(m_job_obs.v6_default_offtunnel)) + "\n";
        s += "lookups            " + std::string(core::canary_name(m_job_obs.canary));
        m_report.set_text(s);

        std::string v = std::string(core::verdict_name(m_job_verdict)) + " — " +
                        core::verdict_text(m_job_verdict);
        for (const auto& n : m_job_detail.notes) v += "\n" + n;
        m_verdict.set_text(v);

        // The one verdict that means "there is a new exit here worth
        // trusting". Asked of `egress_check` rather than decided here -- see
        // the header.
        m_trustable = (m_job_verdict == core::Verdict::ExitUnexpected) &&
                      !m_job_obs.observed_exit.empty();

        if (lg) lg->info("preflight: {}",
                         core::egress_log_ref(m_job_policy, m_job_verdict));
    } else {
        if (m_job_baseline.address.empty()) {
            std::string why = "Could not record it.";
            for (const auto& n : m_job_baseline.notes) why += "\n" + n;
            m_baseline_say.set_text(why);
        } else if (!m_policy.accepted_exits.empty() &&
                   [&] {
                       for (const auto& e : m_policy.accepted_exits)
                           if (core::addr_same(e, m_job_baseline.address)) return true;
                       return false;
                   }()) {
            // What came back is already on the trusted list, which means the
            // tunnel was up: this is the tunnel's exit, not the user's own
            // address. Refusing beats recording a number that would make every
            // future check refuse.
            m_baseline_say.set_text(
                "That came back as one of your trusted exits, which means the "
                "tunnel was still up. Turn it off and try again.");
        } else {
            m_policy.naked_exit = m_job_baseline.address;
            // What it was recorded ON, written at the same moment for the same
            // reason the resolver is: it is one fact about one act. Step 2's
            // only way to notice a tunnel that is not up.
            m_policy.naked_device = m_job_iface;

            // The lookup half APPENDS, and de-duplicates. Appending is the
            // whole reason this is a list: a provider answers from a pool, and
            // a second press on another day is how the second one gets seen.
            // Replacing would make every press throw away the protection the
            // last one bought.
            std::string said;
            if (!m_job_baseline.resolver.empty()) {
                bool known = false;
                for (const auto& rz : m_policy.naked_resolvers)
                    if (core::addr_same(rz, m_job_baseline.resolver)) { known = true; break; }
                if (known) {
                    said = " The resolver that answered is one already recorded, "
                           "so nothing was added — that is a normal result and "
                           "not a failure.";
                } else {
                    m_policy.naked_resolvers.push_back(m_job_baseline.resolver);
                    said = " The resolver that answered your lookup was recorded "
                           "too, which is what the checked-every-time mode needs.";
                }
            } else {
                said = " Your lookups could not be checked this time, so the "
                       "checked-every-time mode has nothing new to go on.";
            }

            std::string v =
                "Recorded. It is stored on this computer and is never shown, "
                "logged, or sent anywhere — delr keeps it so it can recognise "
                "it and refuse." + said;
            for (const auto& n : m_job_baseline.notes) v += "\n" + n;
            m_baseline_say.set_text(v);
            // Counts, never addresses, exactly as `egress_log_ref` does it.
            if (lg) lg->info("baseline recorded: exit yes, resolvers {}",
                             m_policy.naked_resolvers.size());
        }
    }
    refresh();
}

// ── the small acts ───────────────────────────────────────────────────────────

// ── the guided steps ─────────────────────────────────────────────────────────
// Both run on the MAIN THREAD, deliberately, and that is not an oversight of
// the worker rule two sections down. `route_device` sends nothing and waits for
// nothing: it is a route lookup, a `getsockname` and a `getifaddrs`, all of
// which return in microseconds. A worker would buy nothing and cost the thing
// these buttons exist to give -- an answer that appears the instant they are
// pressed. The rule is "do not freeze the window", not "everything touching a
// socket goes off-thread".
//
// Neither one commits anything. They write a name into a visible box; the
// button beside it is still the thing that acts.

void EgressDialog::on_detect_naked() {
    const core::DeviceFound found = net::route_device(false);

    switch (found.state) {
        case core::Detect::Found:
            break;
        case core::Detect::NoRoute:
            m_baseline_say.set_text(
                "This computer has no way out to the internet at the moment, so "
                "there is nothing to find. Connect normally — with the VPN off — "
                "and try again.");
            return;
        case core::Detect::Ambiguous:
            m_baseline_say.set_text(
                "More than one connection claims the address this computer would "
                "send from, so there is no single right answer to fill in. Type "
                "the name of your ordinary connection instead.");
            return;
        case core::Detect::Unmatched:
        case core::Detect::NotRun:
            m_baseline_say.set_text(
                "Could not work out which connection this computer would use. "
                "Type its name instead — the list under the tunnel box below "
                "shows what this computer has.");
            return;
    }

    // The one thing that would make this the WRONG name: the VPN is on, so the
    // device carrying traffic is the tunnel, and recording through it would
    // store the tunnel's exit as this machine's own address -- after which
    // ExitNaked fires on a good tunnel forever. Two ways to notice, and both
    // are cheap. This is the first; `on_record_baseline` still refuses on the
    // second, because a user may type past this.
    if (!m_iface.get_text().empty() &&
        found.device == std::string(m_iface.get_text())) {
        m_baseline_say.set_text(
            "That found “" + found.device + "”, which is the name you have given "
            "for your tunnel — so the VPN looks like it is ON. Turn it off and "
            "press Find it again. This step is the one that has to happen "
            "without it.");
        return;
    }

    m_baseline_iface.set_text(found.device);   // refresh() runs off the change
    m_baseline_say.set_text(
        "Found “" + found.device + "” — that is what this computer is using "
        "right now. If your VPN is off, press Record.");
}

void EgressDialog::on_detect_tunnel() {
    const core::DeviceFound v4 = net::route_device(false);

    switch (v4.state) {
        case core::Detect::Found:
            break;
        case core::Detect::NoRoute:
            m_iface_say.set_text(
                "This computer has no way out to the internet at the moment. "
                "Turn the VPN on and try again.");
            return;
        case core::Detect::Ambiguous:
            m_iface_say.set_text(
                "More than one connection claims the address this computer would "
                "send from, so there is no single right answer to fill in. Type "
                "your tunnel's name instead.");
            return;
        case core::Detect::Unmatched:
        case core::Detect::NotRun:
            m_iface_say.set_text(
                "Could not work out which connection this computer would use. "
                "Type your tunnel's name instead — the list below shows what "
                "this computer has.");
            return;
    }

    // The guard, and the reason step 1 records a device name. Asked of
    // `core::tunnel_check` rather than compared here: it is a judgment, and the
    // dialog decides nothing.
    if (core::tunnel_check(v4, m_policy.naked_device) == core::TunnelCheck::NotUp) {
        m_iface_say.set_text(
            "That found “" + v4.device + "”, which is the very connection you "
            "recorded in step 1 with the VPN off — so the VPN does not look like "
            "it is on. Nothing was filled in. Turn it on and press Find it "
            "again.");
        return;
    }

    m_iface.set_text(v4.device);
    std::string said = "Found “" + v4.device + "”.";
    if (m_policy.naked_device.empty()) {
        // Honest about what was not checked. Step 1 is what makes step 2 able
        // to catch a VPN that is off, and skipping it is allowed.
        said += " Step 1 has not been recorded, so delr cannot tell whether "
                "your VPN is actually on — check that it is.";
    }

    // ── the sibling ──────────────────────────────────────────────────────────
    // The s10 refusal in one button. A provider that carries v6 on a second
    // device is a normal configuration and delr read it as a leak, refusing a
    // machine entirely inside its tunnel. The name is proposed, appended, and
    // shown -- not applied silently -- because a name in this list stops a leak
    // being reported on that device, which is the one direction a wrong entry
    // is quiet in.
    const core::DeviceFound v6 = net::route_device(true);
    if (v6.state == core::Detect::NoRoute) {
        said += " There is no IPv6 route out of this computer, so nothing can "
                "leak that way.";
    } else if (v6.state == core::Detect::Found && v6.device != v4.device) {
        auto devs = devs_split(m_devs.get_text());
        bool known = false;
        for (const auto& d : devs) if (d == v6.device) { known = true; break; }
        if (known) {
            said += " IPv6 goes out “" + v6.device + "”, which is already "
                    "listed as part of this tunnel.";
        } else {
            devs.push_back(v6.device);
            m_devs.set_text(devs_join(devs));
            said += " IPv6 goes out a second device, “" + v6.device + "”, so it "
                    "has been added below as part of this tunnel. Remove it if "
                    "it is not — a name in that list stops delr reporting a leak "
                    "on that device.";
        }
    } else if (v6.state == core::Detect::Found) {
        said += " IPv6 goes out the same device.";
    }

    m_iface_say.set_text(said);
}

void EgressDialog::on_trust_exit() {
    if (!m_trustable || m_job_obs.observed_exit.empty()) return;
    m_policy.accepted_exits.push_back(m_job_obs.observed_exit);
    m_trustable = false;
    repaint_exits();
    m_exits_say.set_text("Trusted. Run the preflight again to confirm it passes.");
    refresh();
}

void EgressDialog::on_forget_exit(const std::string& addr) {
    for (auto it = m_policy.accepted_exits.begin();
         it != m_policy.accepted_exits.end(); ++it) {
        if (*it == addr) { m_policy.accepted_exits.erase(it); break; }
    }
    repaint_exits();
    refresh();
}

void EgressDialog::on_forget_baseline() {
    m_policy.naked_exit.clear();
    // The device goes with it. It is a note about how that address was
    // obtained, and keeping it after the address is gone would leave step 2
    // checking itself against an act that no longer exists.
    m_policy.naked_device.clear();
    m_baseline_say.set_text(
        "Forgotten. Nothing can be checked until it is recorded again with the "
        "tunnel off.");
    refresh();
}

// Separate from the one above, because they are separate protections and a
// user clearing a stale home address after moving house should not silently
// lose every resolver they have ever sampled -- which would leave
// `SystemVerified` refusing with no obvious cause.
void EgressDialog::on_forget_resolvers() {
    m_policy.naked_resolvers.clear();
    m_baseline_say.set_text(
        "Forgotten. The checked-every-time mode will refuse until lookups are "
        "recorded again with the tunnel off.");
    refresh();
}

void EgressDialog::on_save() {
    // Saving a policy that does not validate is allowed on purpose: a
    // half-configured tunnel is a normal state to leave the window in, the
    // problems are shown plainly above the button, and every one of them is
    // refused at fetch time anyway. A settings window that will not let you
    // stop halfway is a settings window people abandon with nothing saved.
    m_policy = harvest();
    m_saved.emit(m_policy);
    set_visible(false);
}

}  // namespace delr
