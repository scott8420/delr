// Work -- what runs off the main thread, and what happens when it lands.
//
// ─────────────────────────────────────────────────────────────────────────────
// THE CHECK. Every piece of this has existed for sessions and none of them were
// wired to anything a person could press: `core/Egress` decides whether a
// request may leave, `net/Observer` produces the evidence for that decision,
// `net/Fetch` carries the request, `core/PageRules` reads the page, and
// `core/Case` records what was seen. This file is the button, and it is
// deliberately nothing else -- every judgment below belongs to one of those
// modules and is ASKED rather than repeated here.
//
// The order, and why it is that order:
//
//   1. preflight        -- gather fresh evidence about the tunnel
//   2. fetch            -- which runs `egress_check` ITSELF and refuses with a
//                          verdict. The gate is not something this file
//                          remembers to do; it is the function it calls.
//   3. page_check       -- what the page says, if there was one to read
//   4. apply            -- `apply_egress_refusal` or `apply_page_verdict`
//   5. promotion        -- believing it gone, or noticing it came back
//
// ── One case, not a run ──────────────────────────────────────────────────────
// The button acts on the SELECTED row. Fifty listings, rate-limited, resumable
// and cancellable is a different piece of work with a different shape (s10-11),
// and pretending this is that would be building the queue by accident.
//
// ── The page never crosses the thread boundary ───────────────────────────────
// `page_check` runs ON THE WORKER, so the broker's page about the user -- the
// most sensitive string this program ever holds -- lives in one stack frame and
// dies with it. What comes back to the main thread is a verdict. The listing
// url is cleared the moment the job lands, for the same reason.
//
// ── A fresh preflight every time ─────────────────────────────────────────────
// The policy has a preflight TTL and `egress_check` judges staleness against
// it, so a cached observation would be legitimate -- and deciding HERE whether
// the cached one is still good would be a second staleness rule, in the file
// least equipped to own one. One case per press makes the cost two extra round
// trips, which nobody will notice. A run over fifty listings will, and that is
// the session that gets to reopen this.
//
// ── No needles yet ───────────────────────────────────────────────────────────
// `PageNeedles` comes from the profile, which is s9-s10. Until then it is
// empty, so a rule with `needs_needle` returns `NoNeedles` -- Indeterminate,
// never NotFound, and straight into the maintenance queue. That is the honest
// state: those brokers cannot be verified yet, and the window says so rather
// than counting them as clean.
// ─────────────────────────────────────────────────────────────────────────────
#include "Shell.hpp"
#include "Log.hpp"

#include "net/Observer.hpp"

#include <glibmm/miscutils.h>
#include <gtkmm/listboxrow.h>

#include <chrono>
#include <filesystem>

namespace delr {
namespace {

// The core has no clock, here as everywhere: `Case` takes `today`, the egress
// judgment takes `now_s`. This is the caller supplying it.
std::int64_t now_monotonic_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

// ── The one gate on the button ───────────────────────────────────────────────
// Three conditions, none of them a judgment of this file's own: something is
// selected, the validator had no complaints about the policy, and the single
// job slot is free. A window that offered a check with no tunnel configured
// would be offering something that cannot happen.
void Shell::refresh_check_button() {
    const bool selected = m_cases_list.get_selected_row() != nullptr;
    if (m_check_action)
        m_check_action->set_enabled(selected && m_egress_ok && !checking());

    if (checking())
        m_check_button.set_tooltip_text("A check is already running.");
    else if (!m_egress_ok)
        m_check_button.set_tooltip_text(
            "The tunnel is not set up yet — see “Tunnel and privacy” in the "
            "menu. Checks go out through a tunnel or they do not go out.");
    else if (!selected)
        m_check_button.set_tooltip_text("Pick a case below first.");
    else
        m_check_button.set_tooltip_text(
            "Fetch the selected listing through the tunnel and record what the "
            "page says");
}

void Shell::on_check_now() {
    if (checking()) return;
    auto* row = m_cases_list.get_selected_row();
    if (!row) return;
    const int i = row->get_index();
    if (i < 0 || static_cast<std::size_t>(i) >= m_caseload.size()) return;
    start_check(m_caseload[static_cast<std::size_t>(i)]);
}

// ── The job slot ─────────────────────────────────────────────────────────────
// The shape EgressDialog built for its preflight, copied as that header asked:
// one slot, the button insensitive while it is occupied, the worker handed
// copies and touching no widget. If a third caller ever wants it, lift it out
// rather than writing it a third time.
void Shell::start_check(const core::Case& k) {
    if (checking()) return;
    if (m_check_worker.joinable()) m_check_worker.join();   // the previous, finished

    m_job_case_id = k.id;
    m_job_url     = k.url;          // PII, and the reason this member is cleared below
    m_job_policy  = m_egress;

    // The rule is copied INTO the job rather than pointed at: `m_rules` can be
    // reloaded from the menu while a check is in flight, and a pointer into a
    // vector that has been reassigned is the kind of bug that only shows up on
    // the machine that is not mine.
    const core::PageRule* rule = core::rules_find(m_rules, k.broker_id);
    m_job_has_rule = (rule != nullptr);
    m_job_rule     = rule ? *rule : core::PageRule{};

    m_job_obs     = core::EgressObservation{};
    m_job_verdict = core::Verdict::Unconfigured;
    m_job_error   = net::FetchError::NotBuilt;
    m_job_ours    = true;                        // pessimistic until judged
    m_job_page    = core::PageVerdict::NoResponse;
    m_job_ms      = 0;

    m_checking = true;
    refresh_check_button();      // insensitive HERE, on the main thread
    m_check_state.set_text(
        "Checking… preflighting the tunnel first, then fetching the page.");

    m_check_worker = std::thread([this] {
        const std::int64_t now = now_monotonic_s();

        m_job_obs     = net::preflight(m_job_policy, net::observer_config(m_job_policy), now);
        m_job_verdict = core::egress_check(m_job_policy, m_job_obs, now);

        net::FetchRequest req;
        req.url = m_job_url;

        const net::FetchResult r = net::fetch(req, m_job_policy, m_job_obs, now);
        m_job_error = r.error;
        m_job_ms    = r.elapsed_ms;

        // A refusal carries the verdict that caused it, which may be sharper
        // than the one the preflight produced a moment earlier -- the tunnel
        // can drop between the two. The refusal's own reason wins.
        if (r.error == net::FetchError::Refused) m_job_verdict = r.verdict;

        m_job_ours = !net::fetch_ok(r) && net::fetch_error_is_ours(r.error);

        // Judged here, on the worker, so `r.body` never leaves this frame.
        if (!m_job_ours)
            m_job_page = core::page_check(m_job_has_rule ? &m_job_rule : nullptr,
                                          r.status, r.body, core::PageNeedles{});

        m_check_done.emit();
    });
}

// ── What came back ───────────────────────────────────────────────────────────
void Shell::on_check_done() {
    m_checking = false;
    if (m_check_worker.joinable()) m_check_worker.join();
    m_job_url.clear();     // PII: held only as long as the fetch needed it

    auto lg = log::get(log::Area::Cases);

    // Find the case AGAIN, by id. The caseload may have moved underneath while
    // the worker was out -- a reload from the menu, a case added -- and writing
    // back to a remembered index would write the result onto somebody else's
    // listing. Same reasoning as re-minting an id at commit time.
    std::size_t at = m_caseload.size();
    for (std::size_t i = 0; i < m_caseload.size(); ++i)
        if (m_caseload[i].id == m_job_case_id) { at = i; break; }

    if (at == m_caseload.size()) {
        if (lg) lg->warn("check: case {} is gone; nothing recorded", m_job_case_id);
        m_check_state.set_text(
            "That case is no longer in the caseload, so nothing was recorded.");
        refresh_check_button();
        return;
    }

    const auto* b = core::roster_find(m_roster, m_caseload[at].broker_id);
    const std::string who = b ? b->name : m_caseload[at].broker_id;
    // The broker's own rhythm for how often a removal is worth re-verifying.
    // A missing broker is a caseload validation problem, already logged; 45 is
    // the schema's own default rather than a number invented here.
    const int recheck = b ? b->recheck_days : 45;

    std::string said;
    if (m_job_ours) {
        // Not a check. `apply_egress_refusal` records Indeterminate/NoTunnel,
        // leaves the failure streak alone -- our outage is not evidence about
        // the listing -- and comes back tomorrow rather than in 45 days.
        m_caseload[at] = core::apply_egress_refusal(m_caseload[at], today());
        said = (m_job_error == net::FetchError::Refused)
                   ? core::verdict_text(m_job_verdict)
                   : net::fetch_error_text(m_job_error);
    } else {
        m_caseload[at] =
            core::apply_page_verdict(m_caseload[at], m_job_page, today(), recheck);
        said = core::page_verdict_text(m_job_page);
    }

    // ── Believing it, which is a separate act ────────────────────────────────
    // `apply_*` above recorded what was SEEN. This is the judgment about what
    // to believe, and it is `promotion_for`'s, made from what is now on the
    // case rather than from what this check happened to see.
    std::string news;
    const core::Promotion prom = core::promotion_for(m_caseload[at]);
    if (prom == core::Promotion::Removed) {
        m_caseload[at] = core::apply_promotion(m_caseload[at]);
        news = "  Believed removed — on our own fetch of the live page, not on "
               "anyone's claim.";
    } else if (prom == core::Promotion::Returned) {
        // The event the whole app exists to catch. Two rows, never an edit:
        // the old case ends at Relisted and a successor opens, and
        // `caseload_record_return` does both halves in one call because a
        // successor without a closed predecessor counts one listing twice.
        std::string fresh_id;
        if (core::caseload_record_return(m_caseload, m_job_case_id, today(),
                                         recheck, &fresh_id)) {
            news = "  It is back. The old case is closed as relisted and " +
                   fresh_id + " has been opened in its place.";
            if (lg) lg->warn("check: {} relisted, successor {}",
                             m_job_case_id, fresh_id);
        }
    }

    // The log gets an id, a named verdict and an outcome. No url, no page, no
    // address -- `egress_log_ref` and the *_name functions are the only things
    // in this program that may describe a check in a file.
    if (lg)
        lg->info("check: case:{} {} {} -> {}/{}", m_job_case_id,
                 core::egress_log_ref(m_job_policy, m_job_verdict),
                 m_job_ours ? net::fetch_error_name(m_job_error)
                            : core::page_verdict_name(m_job_page),
                 core::outcome_name(m_caseload[at].outcome),
                 core::reason_name(m_caseload[at].reason));

    const std::string file = cases_file();
    std::error_code ec;
    std::filesystem::create_directories(Glib::path_get_dirname(file), ec);

    if (!core::caseload_save(file, m_caseload)) {
        // The result is in memory and not on disk. Say so plainly: silently
        // losing a check is worse than not having run one, because the next
        // run will believe it is the first.
        if (lg) lg->error("check: save failed");
        m_check_state.set_text(who + ": " + said +
                               "  This result could NOT be saved — see the log.");
        refresh_check_button();
        return;
    }

    // Repaint FROM DISK, as the intake path does: the reload proves the write
    // round-tripped, so the row and the file agree by construction rather than
    // by assumption. It also restores the selection, so the same case can be
    // checked again without hunting for it.
    on_reload_cases();
    m_check_state.set_text(who + ": " + said + news);
}

}  // namespace delr
