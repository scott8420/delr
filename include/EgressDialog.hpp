#pragma once
#include "widgets/Widgets.hpp"
#include "core/Egress.hpp"
#include "net/Observer.hpp"

#include <gtkmm/window.h>
#include <gtkmm/stringlist.h>
#include <glibmm/dispatcher.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// EgressDialog -- where the tunnel gets configured, and the last thing standing
// between the fetch and a button.
//
// `core/Egress` has been able to decide since s4 whether a request may leave.
// `net/Fetch` and `net/Observer` have been able to produce the evidence since
// s6. Nobody has been able to TELL it any of this: the policy had no window and
// no file, so the only working configuration path was `--netcheck` with the
// interface on the command line. This window and `egress_policy_load/save` are
// the two halves of that gap.
//
// ── THE DIALOG DECIDES NOTHING, and here that is load-bearing ────────────────
// Same rule as AddCaseDialog, with more at stake. Every judgment on this
// surface is somebody else's: is the policy coherent (`egress_policy_validate`),
// is the proxy usable (`proxy_url_ok`), does the tunnel pass right now
// (`egress_check`), can this library pin a resolver (`fetch_can_pin_dns`). This
// file reads those answers and arranges labels around them. The moment a rule
// gets written here there are two policies in the program and one of them is
// not tested.
//
// The clearest example is the "Trust this exit" button, which is enabled by a
// VERDICT and not by a comparison written here: it turns on only for
// `ExitUnexpected` -- public, not the machine's own address, not already
// trusted. `egress_check` was already computing exactly that, and asking it is
// the difference between one definition of "a new exit" and two.
//
// ── What this window shows, and the one thing it will not ────────────────────
// It shows the accepted exit list in full, because the user has to be able to
// tell which entry to remove, and because those are a VPN provider's addresses
// rather than the user's.
//
// It never shows `naked_exit`. Not once, not masked, not on hover. That is this
// machine's home address -- the single identifier the whole app exists to keep
// away from brokers -- and it is recorded by a button and displayed as the word
// "recorded". Nobody needs to read it back; the policy needs it, and the policy
// is not a person. Same discipline as `--netcheck` printing no addresses and
// `egress_log_ref()` carrying none.
//
// The same goes for `naked_resolvers`, with one relaxation: the COUNT is shown.
// A count is not an address, and it is the only honest way to tell a user how
// strong the mode they picked actually is -- "seen 1 resolver" and "seen 3" are
// different guarantees, and s9 made pressing Record a second time a thing worth
// doing rather than a mistake.
//
// It also never shows the exit a preflight just saw, for a sharper reason: when
// the tunnel is DOWN that address IS `naked_exit`, so a window that displayed
// "exit seen: …" would print the user's home address exactly when things had
// gone wrong, and invite them to press Trust on it. The verdict is shown
// instead, which says "the tunnel is down" in words.
//
// ── Three numbered steps, and why the window was reordered around them ───────
// s10 opened this window for the first time and it did not survive contact.
// Four walls in sequence before a single preflight could run, hit by the person
// who owns the product, and his sentence at the end was the finding: "I just
// felt I was lost in the process." For a privacy tool that is not a polish
// complaint. A setup path people fall out of is a setup path that leaves them
// with no tunnel at all, which is the outcome the whole app exists to prevent.
// THE SETUP PATH IS A SAFETY FEATURE.
//
// So the window now reads as a sequence instead of a pile of fields:
//
//   1. Your ordinary connection, tunnel OFF -- detect it, record the baseline
//   2. Your tunnel, tunnel ON               -- detect it, and its siblings
//   3. Check it                             -- run the preflight
//
// Everything else -- the lookup mode, the trusted exits, the endpoints, the
// ttl -- is settings, and settings sit below the path. The order is the real
// order of operations, and it is why the baseline block moved ABOVE the tunnel
// block: the baseline has to be recorded with the VPN off, so it genuinely
// comes first, and a window that numbered its steps while presenting them
// backwards would be worse than one that numbered nothing.
//
// ── Detect rather than ask, and why that is not the app deciding ─────────────
// delr deliberately makes the user state things rather than inferring them --
// the Tunnel field is an entry and not a dropdown for exactly that reason, so
// that a tunnel which is DOWN does not vanish out of the configuration. What
// the principle forbids is the app deciding SILENTLY. It does not forbid the
// app proposing.
//
// This window already contained the pattern: "Trust this exit" does not
// auto-populate `accepted_exits`; the preflight finds an exit, the window shows
// it, and a press accepts it. The two detect buttons are that same pattern two
// sections higher. They fill a visible box a person can read, correct, or
// ignore, and nothing is committed until the button beside it is pressed.
//
// Wall 2 is what this fixes. The baseline entry showed placeholder `eth0` in
// dim grey -- the same grey as a disabled button -- directly above two fields
// that WERE prefilled, so the box read as full and Record read as broken. The
// fix is not to restyle the placeholder. It is to put a real name in the box.
//
// ── Every action reports where it lives ──────────────────────────────────────
// Wall 3, and the general rule s10 asked for. This window used to have ONE
// verdict sink, at the bottom: Record's refusal ("that is the tunnel") rendered
// a full screen below the button that had just been pressed, so pressing Record
// appeared to do nothing at all. The logic was right and the sentence landed
// somewhere the user was not looking, which is the pattern all four walls
// shared.
//
// So each action now has a sink beside it -- `m_baseline_say` under step 1,
// `m_iface_say` under step 2, `m_exits_say` under the trusted-exit list -- and
// `m_verdict` belongs to the preflight alone, which is the one action that was
// already adjacent to it. A new action gets a new label next to itself; it does
// not get a line appended to the bottom of the window.
//
// ── Work off the main thread ─────────────────────────────────────────────────
// A preflight is two round trips over a tunnel with a 20s budget, and a window
// frozen for twenty seconds is a window the user force-quits. So the two
// network jobs here -- the preflight and recording the baseline -- run on a
// worker thread and come back through a `Glib::Dispatcher`.
//
// One job at a time, by construction: there is a single worker slot and the
// buttons go insensitive while it is occupied. The worker touches no widget and
// no shared policy -- it is handed a copy, it fills in a result, it emits. Every
// widget in this file is touched on the main thread only.
//
// s8's "Check now" wants precisely this shape. It should be copied from here
// rather than reinvented, and if a third caller appears it should be lifted out
// instead.
//
// Lifetime: hide on close (the Cairn pattern), a member of the Shell, built
// once. Hiding CLEARS the working policy, because it holds `naked_exit` and a
// hidden window holding the user's home address is the same failure as a log
// file holding it.
// ─────────────────────────────────────────────────────────────────────────────
namespace delr {

class EgressDialog : public Gtk::Window {
public:
    EgressDialog();
    ~EgressDialog() override;

    // Open against a COPY of the policy. The Shell owns the file and the
    // on-disk truth; this window edits a copy and hands one back, the same seam
    // that keeps AddCaseDialog from ever touching cases.json.
    void open(Gtk::Window& parent, const core::EgressPolicy& p);

    // Emitted with the policy to persist. The dialog never writes a file.
    sigc::signal<void(core::EgressPolicy)>& signal_saved() { return m_saved; }

private:
    void build();                      // widgets and layout
    void bind();                       // signals
    void refresh();                    // repaint every derived label

    core::EgressPolicy harvest() const;          // widgets -> policy
    void               plant(const core::EgressPolicy& p);   // policy -> widgets
    void               repaint_exits();
    core::DnsMode      chosen_mode() const;

    void on_save();
    void on_run_preflight();
    void on_record_baseline();
    void on_forget_baseline();
    void on_forget_resolvers();
    void on_trust_exit();
    void on_forget_exit(const std::string& addr);
    void on_detect_naked();
    void on_detect_tunnel();
    void on_worker_done();
    void on_hidden();

    // ── The worker slot ──────────────────────────────────────────────────────
    enum class Job { None, Preflight, Baseline };
    void start(Job j);
    bool busy() const { return m_job != Job::None; }

    Job              m_job = Job::None;
    std::thread      m_worker;
    Glib::Dispatcher m_done;

    // Written by the worker, read on the main thread after `m_done` fires.
    // Nothing else crosses.
    core::EgressPolicy  m_job_policy;    // the copy the worker was handed
    std::string         m_job_iface;     // for a baseline job
    core::EgressObservation m_job_obs;
    net::Observation        m_job_detail;
    core::Verdict           m_job_verdict = core::Verdict::Unconfigured;
    net::BaselineResult     m_job_baseline;

    // True when the last preflight ended on `ExitUnexpected` -- the one verdict
    // that means "there is a new exit here worth trusting". Not an address: the
    // address is carried in `m_job_obs` and never shown.
    bool m_trustable = false;

    // `m_frame` is what the window holds: the scroller, then the button row,
    // pinned. `m_root` is what scrolls. See the constructor for why the buttons
    // sit outside.
    widgets::Box   m_frame, m_root, m_iface_row, m_dns_row, m_proxy_row,
                   m_resolver_row, m_ttl_row, m_exits_head, m_baseline_row,
                   m_preflight_row, m_echo_row, m_canary_row, m_devs_row,
                   m_buttons;
    widgets::Label m_intro, m_iface_label, m_iface_live, m_devs_label, m_devs_note, m_dns_label, m_dns_note,
                   m_proxy_label, m_proxy_state, m_resolver_label, m_resolver_note,
                   m_ttl_label, m_exits_label, m_baseline_label, m_baseline_state,
                   m_resolver_state, m_endpoints_label, m_echo_label, m_canary_label,
                   m_endpoints_note, m_report, m_verdict, m_problems,
                   // The three step headings, and the three LOCAL sinks that go
                   // with them. See the block above about where a sentence has
                   // to land.
                   m_step1, m_step2, m_step3,
                   m_baseline_say, m_iface_say, m_exits_say;
    widgets::Entry m_iface, m_devs, m_proxy, m_resolver, m_ttl, m_baseline_iface,
                   m_echo_url, m_canary_url;
    widgets::DropDown       m_dns;
    widgets::ListBox        m_exits;
    widgets::ScrolledWindow m_exits_scroll, m_scroll;
    widgets::Button m_trust, m_record, m_forget_baseline, m_forget_resolvers,
                    m_preflight, m_close, m_save, m_detect_naked, m_detect_tunnel;

    Glib::RefPtr<Gtk::StringList> m_dns_names;

    // The policy being edited. Cleared on hide -- it holds `naked_exit`.
    core::EgressPolicy m_policy;

    bool m_setting = false;   // guards programmatic widget updates
    sigc::signal<void(core::EgressPolicy)> m_saved;
};

}  // namespace delr
