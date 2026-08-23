// Handlers -- slot bodies. What a user action actually does.
#include "Shell.hpp"
#include "Registry.hpp"
#include "Log.hpp"
#include "netcheck.hpp"

#include <glibmm/miscutils.h>
#include <glibmm/datetime.h>
#include <gtkmm/label.h>
#include "Paths.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace delr {

// Path resolution moved to `paths` in s12; these three stay as members so
// every call site in the Shell reads the same as it did, and so the core still
// never learns where a file came from. What changed is underneath: the roster
// and the rules are ASSETS and still sit beside the program, while the
// caseload and the policy are STATE and no longer do.
std::string Shell::roster_file() const { return paths::roster_file(); }

// Quit by closing the window rather than by asking the application to quit:
// the destructor is what hides the two member dialogs and joins the worker, and
// close() is what runs it. `Gtk::Application` exits when its last window goes.
void Shell::on_quit() { close(); }

void Shell::on_reload_roster() {
    const std::string file = roster_file();
    std::string err;
    m_roster = core::roster_load(file, &err);

    auto lg = log::get(log::Area::Roster);
    if (lg) {
        if (!err.empty()) lg->error("roster parse failed: {}", err);
        else              lg->info("roster loaded: {} broker(s)", m_roster.size());
    }

    // Validation is loud by design -- a bad roster is a bug in the DATA, and
    // the data is the asset (CANON: dead mapping entries lie about structure).
    const auto problems = core::roster_validate(m_roster);
    for (const auto& p : problems) if (lg) lg->warn("roster: {}", p);

    // Repaint the list.
    while (auto* row = m_roster_list.get_row_at_index(0)) m_roster_list.remove(*row);
    for (const auto& b : m_roster) {
        auto* row = Gtk::make_managed<Gtk::Label>(
            b.name + "   [" + core::method_name(b.method) + "]" +
            (b.requires_id ? "  (photo ID)" : "") +
            (b.ca_registered ? "  (CA registered)" : ""));
        row->set_xalign(0.0f);
        row->set_margin(6);
        m_roster_list.append(*row);
    }

    // The caption under the explainer, and it now says what the table IS and
    // what its tags MEAN. The tags were the last unexplained thing on the
    // page: `[web]` and `[drop]` are the difference between "fill in a form"
    // and "one state platform does it for every broker registered there", and
    // a user who cannot tell them apart cannot do step 2.
    //
    // The absolute path moved to the tooltip. It is the right answer to "which
    // file is this" and the wrong opening line for a page explaining what the
    // program is -- this is an ASSET, the same on every machine, so its
    // location is a maintenance detail rather than something to lead with.
    if (!err.empty()) {
        m_roster_status.set_text("Roster failed to parse: " + err);
    } else {
        m_roster_status.set_text(
            std::to_string(m_roster.size()) +
            " broker(s) delr knows how to reach.   [web] a form you fill in "
            "   [email] a message you send   [drop] California's platform, "
            "which relays to every broker registered there." +
            (problems.empty() ? "" : "   --   " + std::to_string(problems.size()) +
                                     " validation problem(s), see log"));
    }
    m_roster_status.set_tooltip_text(file);
}

// The caseload's path. Unlike the roster this file is PII -- it is a list of
// pages that expose this user, by URL -- so it lives under XDG rather than in
// the tree, and when encryption-at-rest lands it lands behind
// core::caseload_load and this line does not change.
std::string Shell::cases_file() const { return paths::cases_file(); }

// Table three's path. An asset like the roster, and its own file because it
// rots on a designer's schedule rather than a company's.
std::string Shell::rules_file() const { return paths::rules_file(); }

void Shell::on_reload_rules() {
    const std::string file = rules_file();
    std::string err;
    m_rules = core::rules_load(file, &err);

    auto lg = log::get(log::Area::Cases);
    if (lg) {
        if (!err.empty()) lg->error("page rules parse failed: {}", err);
        else              lg->info("page rules loaded: {} rule(s)", m_rules.size());
    }

    // Validated AGAINST THE ROSTER, which is the whole reason this reload runs
    // after that one: a rule naming a broker nobody has is a rule that will
    // never fire, and it is the same class of bug as a dead mapping entry.
    const auto problems = core::rules_validate(m_rules, &m_roster);
    for (const auto& p : problems) if (lg) lg->warn("rules: {}", p);

    // No status line of its own. A rule is not a thing the user does anything
    // with; what they need to know is which of THEIR cases cannot be read, and
    // that sentence is the maintenance line on the cases page.
    if (!m_caseload.empty()) on_reload_cases();
}

// The clock lives on the UI side. core::Case does date ARITHMETIC and never
// asks what day it is -- a "pure" function that reads the clock isn't pure, and
// "which cases are due" would stop being testable the moment the core could
// answer that itself. The UI knows today; the core knows how to count.
std::string Shell::today() const {
    return Glib::DateTime::create_now_local().format("%Y-%m-%d");
}

// One row of display text. The URL's HOST only, never the full path -- the path
// is the part carrying the name ("/John-Smith/Tennessee/..."), and a screenshot
// of this window is a thing that happens.
std::string Shell::case_row_text(const core::Case& k) const {
    const auto* b = core::roster_find(m_roster, k.broker_id);
    std::string who = b ? b->name : k.broker_id;

    // The host comes from core::url_host now that the intake path needs the
    // same answer -- one parser, so the row and the matcher can never disagree
    // about what site a listing is on.
    const std::string host = core::url_host(k.url);

    std::string line = who + "   [" + core::status_name(k.status) + "]";

    // Provenance is shown ONLY for a removed case, and it is shown plainly,
    // because the difference between "they say so" and "we looked" is the
    // entire reason this app exists.
    if (k.status == core::Status::Removed)
        line += "  (" + std::string(core::provenance_name(k.provenance)) + ")";

    line += "   last check: ";
    if (k.outcome == core::Outcome::Never) {
        line += "never";
    } else {
        line += core::outcome_name(k.outcome);
        if (k.outcome == core::Outcome::Indeterminate)
            line += " (" + std::string(core::reason_name(k.reason)) + ")";
        if (!k.last_attempt.empty()) line += " on " + k.last_attempt;
    }
    if (k.consecutive_failures > 0)
        line += "  [" + std::to_string(k.consecutive_failures) + " failed]";
    // The evidence toward believing it gone, shown next to the evidence
    // against. One clean absence is an event and two is a pattern, and a row
    // that shows only "not found" hides which of those this is.
    if (k.clean_absences > 0)
        line += "  [" + std::to_string(k.clean_absences) + " clean]";
    if (!k.next_check.empty()) line += "   due " + k.next_check;
    line += "   " + host;
    return line;
}

void Shell::on_reload_cases() {
    const std::string file = cases_file();
    std::string err;
    m_caseload = core::caseload_load(file, &err);

    auto lg = log::get(log::Area::Cases);
    if (lg) {
        if (!err.empty()) lg->error("caseload parse failed: {}", err);
        else              lg->info("caseload loaded: {} case(s)", m_caseload.size());
    }

    const auto problems = core::caseload_validate(m_caseload);
    for (const auto& p : problems) if (lg) lg->warn("caseload: {}", p);

    // Per-case logging goes through log_ref() and nothing else. The row text
    // above is for the window; the log gets an id and an outcome.
    for (const auto& k : m_caseload)
        if (lg) lg->debug("{} status={} outcome={}", core::log_ref(k),
                          core::status_name(k.status), core::outcome_name(k.outcome));

    // Which case was selected, by ID rather than by row index: a repaint after
    // a relist has one more row than it started with, and an index would then
    // move the user's selection onto a different listing without telling them.
    std::string was_selected;
    if (auto* sel = m_cases_list.get_selected_row()) {
        const int i = sel->get_index();
        if (i >= 0 && static_cast<std::size_t>(i) < m_caseload.size())
            was_selected = m_caseload[static_cast<std::size_t>(i)].id;
    }

    while (auto* row = m_cases_list.get_row_at_index(0)) m_cases_list.remove(*row);
    for (const auto& k : m_caseload) {
        auto* row = Gtk::make_managed<Gtk::Label>(case_row_text(k));
        row->set_xalign(0.0f);
        row->set_margin(6);
        m_cases_list.append(*row);
    }

    if (!was_selected.empty()) {
        for (std::size_t i = 0; i < m_caseload.size(); ++i) {
            if (m_caseload[i].id != was_selected) continue;
            if (auto* row = m_cases_list.get_row_at_index(static_cast<int>(i)))
                m_cases_list.select_row(*row);
            break;
        }
    }
    refresh_check_button();

    if (!err.empty()) {
        m_cases_status.set_text("Caseload failed to parse: " + err);
        m_cases_exposure.set_text("");
        m_cases_maintenance.set_text("");
        return;
    }

    if (m_caseload.empty()) {
        // The honest empty state. Discovery is deliberately not automated, so
        // an empty caseload is the correct first-run condition and not an error
        // to apologise for.
        m_cases_status.set_text("No cases yet.");
        m_cases_maintenance.set_text("");
        m_cases_exposure.set_text(
            "Find your own listing in your own browser, then add its URL with + "
            "in the title bar (Ctrl+N). delr does not search brokers for you -- "
            "querying them with your name is a signal to them, not an "
            "observation of them.");
        return;
    }

    const std::string now = today();
    const auto due = core::caseload_due(m_caseload, now);

    int indeterminate = 0;
    for (const auto& k : m_caseload)
        if (k.outcome == core::Outcome::Indeterminate) ++indeterminate;

    std::string status = std::to_string(m_caseload.size()) + " case(s), " +
                         std::to_string(due.size()) + " due as of " + now;
    // Unchecked cases lead the status line rather than hiding in the list. A
    // run where half the checks were blocked must not read as a clean sweep.
    if (indeterminate > 0)
        status += "   --   " + std::to_string(indeterminate) + " unchecked";
    if (!problems.empty())
        status += "   --   " + std::to_string(problems.size()) +
                  " validation problem(s), see log";
    m_cases_status.set_text(status);

    // The roll-up: what of yours is exposed, and on how many live listings.
    const auto by = core::exposure_by_field(m_caseload, false);
    if (by.empty()) {
        m_cases_exposure.set_text("No exposed fields recorded on live cases.");
    } else {
        std::string line = "Exposed:";
        for (std::size_t i = 0; i < by.size(); ++i) {
            line += (i ? ",  " : "  ");
            line += std::string(core::field_name(by[i].field)) + " x" +
                    std::to_string(by[i].count);
        }
        m_cases_exposure.set_text(line);
    }

    // ── The maintenance queue ────────────────────────────────────────────────
    // `caseload_unverifiable()` is the core's own answer to "which of these did
    // we fetch and fail to READ" -- a missing rule, a rotted one, a page that
    // did not fingerprint. Asked rather than recomputed here, because it is
    // the honest denominator behind any count this window prints, and two
    // definitions of "unverifiable" would be one too many.
    //
    // Named by BROKER, not by case: the fix is a rule, and a rule is per
    // broker. A list of case ids would tell the user nothing they can act on.
    // The profile comes FIRST when there isn't one, and that ordering is the
    // point. Without a profile every needle-requiring rule returns NoNeedles,
    // which lands in `caseload_unverifiable` looking exactly like a rotted
    // page rule -- and it is not one. It is the user's own missing half, it is
    // the only entry on this line they can fix themselves, and for six
    // sessions the window blamed the rules for it.
    std::string maintenance;
    if (core::profile_is_empty(m_profile)) {
        maintenance =
            "No profile yet, so delr cannot tell whether a record on a page is "
            "yours -- every check on a per-person listing will come back "
            "unverified rather than clean. Fill in the You page.";
    }

    const auto stuck = core::caseload_unverifiable(m_caseload);
    if (stuck.empty()) {
        m_cases_maintenance.set_text(maintenance);
    } else {
        std::vector<std::string> who;
        for (const core::Case* k : stuck) {
            const auto* b = core::roster_find(m_roster, k->broker_id);
            const std::string name = b ? b->name : k->broker_id;
            if (std::find(who.begin(), who.end(), name) == who.end())
                who.push_back(name);
        }
        std::string line = std::to_string(stuck.size()) +
                           " listing(s) could be fetched but not read -- ";
        for (std::size_t i = 0; i < who.size(); ++i) line += (i ? ", " : "") + who[i];
        line +=
            ". That is a page rule missing or out of date, not an answer about "
            "the listing: those checks count as unverified and never as gone.";
        if (!maintenance.empty()) line = maintenance + "\n" + line;
        m_cases_maintenance.set_text(line);
    }
}

// A row was picked, or unpicked. The only thing that hangs off it is whether
// there is something for the check button to act on.
void Shell::on_case_selected() { refresh_check_button(); }

// ── Intake ───────────────────────────────────────────────────────────────────

void Shell::on_add_case() {
    // Both tables go in by value; the dialog works from a snapshot and hands
    // back a case. It never touches the file.
    m_add_dialog.open(*this, m_roster, m_caseload, today());
}

void Shell::on_case_committed(core::Case fresh) {
    auto lg = log::get(log::Area::Cases);

    // The dialog minted the id against the snapshot it was given. The live
    // caseload is the authority, and it may have moved underneath -- a reload
    // while the dialog was open, a second window later. Re-mint on collision
    // rather than refusing the user's work.
    if (core::caseload_find(m_caseload, fresh.id)) {
        const std::string was = fresh.id;
        fresh.id = core::next_case_id(m_caseload, fresh.broker_id, today());
        if (lg) lg->warn("intake: id {} was taken, minted {}", was, fresh.id);
    }

    if (!core::caseload_commit(m_caseload, fresh)) {
        if (lg) lg->error("intake: commit refused for {}", core::log_ref(fresh));
        m_cases_status.set_text("Could not add that case -- see the log.");
        return;
    }

    // The caseload's directory may not exist yet (DELR_CASES can point
    // anywhere). Creating it is path work, so it is UI-side, like resolving it.
    const std::string file = cases_file();
    std::error_code ec;
    std::filesystem::create_directories(Glib::path_get_dirname(file), ec);

    if (!core::caseload_save(file, m_caseload)) {
        // The case is in memory but not on disk. Say so plainly: a silent
        // failure here loses the one thing the user just did by hand.
        if (lg) lg->error("intake: save failed");
        m_cases_status.set_text("Added, but the caseload could not be saved -- see the log.");
        return;
    }
    if (lg) lg->info("intake: committed {}, caseload now {} case(s)",
                     core::log_ref(fresh), m_caseload.size());

    // Repaint FROM DISK rather than from memory: the reload proves the write
    // round-tripped, and the trace and the window then agree by construction
    // instead of by assumption.
    on_reload_cases();
    m_stack.set_visible_child(m_cases_page);
}

// ── The tunnel policy ────────────────────────────────────────────────────────

// Same shape as the two above, third table. Unlike the roster and like the
// caseload this file is PII -- it holds `naked_exit` -- and the pump writes it
// 0600 for that reason. Path resolution stays here because path resolution is
// the UI's job, which is the same seam that keeps the clock out of the core.
// The absolute-path dance this used to do is gone with the relative default
// that made it necessary: `paths::egress_file` is absolute or it is empty, and
// empty means there is no home to write to, which is a refusal rather than
// something to paper over with the working directory.
std::string Shell::egress_file() const  { return paths::egress_file(); }
std::string Shell::profile_file() const { return paths::profile_file(); }

// ─────────────────────────────────────────────────────────────────────────────
// The profile
//
// Load paints the form; save reads it, validates, writes, and then RELOADS --
// the same round-trip discipline the caseload and the egress policy use, so
// what is on screen is what is on disk rather than what the handler believed it
// wrote.
//
// Nothing here parses. `core::terms_parse` decides what a term is, and this
// file hands it text. The one thing that looks like parsing is the birth year,
// and even that only asks whether the box is a number -- the range check is the
// validator's, in core, where it can be exercised.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::on_reload_profile() {
    std::string err;
    m_profile = core::profile_load(profile_file(), &err);

    auto lg = log::get(log::Area::App);
    if (lg) {
        if (!err.empty()) lg->error("profile parse failed: {}", err);
        // COUNTS, never values. `profile_log_ref` exists so the safe thing is
        // the easy thing -- see its header.
        else              lg->info("{}", core::profile_log_ref(m_profile));
    }

    m_profile_name.set_text(m_profile.full_name);
    m_profile_contact.set_text(m_profile.contact_email);
    m_profile_year.set_text(m_profile.birth_year > 0
                                ? std::to_string(m_profile.birth_year)
                                : std::string());
    m_profile_aka.set_text(core::terms_join(m_profile.also_known_as));
    m_profile_emails.set_text(core::terms_join(m_profile.emails));
    m_profile_phones.set_text(core::terms_join(m_profile.phones));
    m_profile_usernames.set_text(core::terms_join(m_profile.usernames));
    m_profile_places.set_text(core::terms_join(m_profile.places));

    if (!err.empty()) {
        m_profile_status.set_text("The profile file could not be read: " + err);
    } else {
        const auto problems = core::profile_validate(m_profile);
        std::string line = core::profile_summary(m_profile);
        for (const std::string& p : problems) line += "\n" + p;
        m_profile_status.set_text(line);
    }

    // The cases page's account of what a check can currently prove depends on
    // this, so it repaints -- but only once there is a caseload to repaint.
    if (!m_caseload.empty()) on_reload_cases();
}

void Shell::on_save_profile() {
    core::Profile p;
    p.full_name     = m_profile_name.get_text();
    p.contact_email = m_profile_contact.get_text();
    p.also_known_as = core::terms_parse(m_profile_aka.text());
    p.emails        = core::terms_parse(m_profile_emails.text());
    p.phones        = core::terms_parse(m_profile_phones.text());
    p.usernames     = core::terms_parse(m_profile_usernames.text());
    p.places        = core::terms_parse(m_profile_places.text());
    p.note          = m_profile.note;   // not on the form yet; not destroyed by it

    // A blank box is 0 -- "not given" -- and anything else is whatever the user
    // typed, handed to the validator UNCHANGED. Coercing "84" to 1984 here
    // would be the surface making a judgement, and it would be wrong for
    // somebody born in 1884.
    {
        const std::string y = m_profile_year.get_text();
        p.birth_year = 0;
        if (!y.empty()) {
            try { p.birth_year = std::stoi(y); }
            catch (const std::exception&) { p.birth_year = -1; }  // caught below
        }
    }

    const auto problems = core::profile_validate(p);
    auto lg = log::get(log::Area::App);
    if (!problems.empty()) {
        // REFUSED, not saved-with-warnings. Two of these guard invariants the
        // rest of the app will lean on -- an opt-out filed from an address the
        // profile never listed is a request that bounces into a mailbox nobody
        // reads -- and a validator whose complaints can be ignored is a
        // validator that will be.
        std::string line = "Not saved:";
        for (const std::string& e : problems) line += "\n" + e;
        m_profile_status.set_text(line);
        if (lg) lg->warn("profile not saved: {} problem(s)", problems.size());
        return;
    }

    std::string perr;
    if (!paths::ensure_state_dir(&perr)) {
        m_profile_status.set_text("Not saved. " + perr);
        if (lg) lg->error("profile save: {}", perr);
        return;
    }
    if (!core::profile_save(profile_file(), p)) {
        m_profile_status.set_text(
            "The profile could not be written — see the log.");
        if (lg) lg->error("profile save failed");
        return;
    }
    if (lg) lg->info("profile saved: {}", core::profile_log_ref(p));

    // Repaint FROM DISK. The reload proves the write round-tripped, so the
    // trace and the window agree by construction rather than by assumption.
    on_reload_profile();
}

void Shell::on_reload_egress() {
    std::string err;
    m_egress = core::egress_policy_load(egress_file(), &err);

    const auto problems = core::egress_policy_validate(m_egress);
    // The validator's answer, cached for the check button. Whether a check may
    // be OFFERED is the same question as whether the policy is coherent, and
    // that question already has an owner.
    m_egress_ok = err.empty() && problems.empty();

    // The pristine state: nothing has ever been configured. Distinguished from
    // "configured, and wrong" because they are different events and want
    // different levels -- a first launch is not a warning, and a policy that
    // used to work and no longer validates is.
    const bool untouched = m_egress.interface_name.empty() &&
                           m_egress.dns == core::DnsMode::Unset &&
                           m_egress.accepted_exits.empty() &&
                           m_egress.naked_exit.empty() &&
                           // s9's other half of the baseline. A user who
                           // recorded their lookups and nothing else has
                           // configured something, and logging that as a first
                           // launch would file a real state under "pristine".
                           m_egress.naked_resolvers.empty();

    auto lg = log::get(log::Area::App);
    if (lg) {
        if (!err.empty()) {
            lg->error("egress policy parse failed: {}", err);
        } else if (untouched) {
            lg->info("egress policy: none saved yet, so nothing will be checked");
        } else {
            // NOT `egress_log_ref` with a verdict here. That function pairs an
            // interface with a JUDGMENT, and nothing has been judged at load
            // time -- passing `Pass` to fill the slot printed "egress:-/pass"
            // over a policy that refuses everything, which is the one thing a
            // log in this program must never do. The interface name and the
            // mode are both safe to say; neither is an address.
            lg->info("egress policy loaded: interface={} lookups={} problems={}",
                     m_egress.interface_name.empty() ? "-" : m_egress.interface_name,
                     core::dns_mode_name(m_egress.dns), problems.size());
        }
    }

    // The validator already says "egress: " at the front of every line. The
    // area tag is on the left of the log line as well, so a second prefix here
    // made it "egress: egress: ...".
    if (lg && !untouched)
        for (const auto& p : problems) lg->warn("{}", p);

    refresh_check_button();

    if (!err.empty()) {
        m_egress_status.set_text("Tunnel settings failed to parse — see the log.");
    } else if (problems.empty()) {
        m_egress_status.set_text(
            "Tunnel: configured. Checks will run a preflight first and refuse "
            "if it does not pass.");
    } else if (untouched) {
        // The honest first-run state, and not an error to apologise for: a
        // policy nobody has written is a policy that allows nothing, which is
        // the safe reading of "I don't know".
        m_egress_status.set_text(
            "Tunnel: not set up yet, so nothing will be checked. Open “Tunnel "
            "and privacy” in the menu — checks go out through a tunnel or they "
            "do not go out at all.");
    } else {
        m_egress_status.set_text(
            "Tunnel: " + std::to_string(problems.size()) +
            " thing(s) still to sort out before anything can be checked. See "
            "“Tunnel and privacy” in the menu.");
    }
}

void Shell::on_egress_settings() { m_egress_dialog.open(*this, m_egress); }

void Shell::on_egress_saved(core::EgressPolicy p) {
    m_egress = std::move(p);

    const std::string file = egress_file();
    std::error_code ec;
    std::filesystem::create_directories(Glib::path_get_dirname(file), ec);

    auto lg = log::get(log::Area::App);
    if (!core::egress_policy_save(file, m_egress)) {
        if (lg) lg->error("egress policy save failed");
        m_egress_status.set_text(
            "Tunnel settings could not be saved — see the log.");
        return;
    }
    if (lg) lg->info("egress policy saved");

    // Repaint FROM DISK, exactly as the caseload does: the reload proves the
    // write round-tripped, so the trace and the window agree by construction
    // rather than by assumption.
    on_reload_egress();
}

void Shell::on_dump_registry() { registry::dump(); }

}  // namespace delr
