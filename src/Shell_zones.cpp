// Zones -- construction of named regions. No behaviour here.
#include "Shell.hpp"

#include <gtkmm/headerbar.h>
#include <gtkmm/frame.h>
#include <gtkmm/popovermenu.h>

namespace delr {

void Shell::build_shell() {
    set_title("delr");
    set_default_size(900, 600);

    auto* header = Gtk::make_managed<Gtk::HeaderBar>();
    // Adding a case is the app's one creating action, so it gets the one
    // always-visible button. Everything else lives behind the hamburger.
    m_add_button.set_icon_name("list-add-symbolic");
    m_add_button.set_tooltip_text("Add a case from a listing URL (Ctrl+N)");
    m_add_button.set_action_name("win.add-case");
    m_menu_button.set_icon_name("open-menu-symbolic");
    m_menu_button.set_menu_model(build_menu());
    header->pack_start(m_add_button);
    header->pack_end(m_menu_button);
    set_titlebar(*header);

    m_sidebar.set_stack(m_stack);
    m_stack.set_hexpand(true);
    m_stack.set_vexpand(true);

    m_body.append(m_sidebar);
    m_body.append(m_stack);
    set_child(m_body);
}

void Shell::build_pages() {
    // ── Roster: what the app IS, and then the table it reads ────────────────
    //
    // s12 opened the app for the first time with the setup path behind it and
    // the finding was "I don't feel an understanding of what is going on and
    // how to use the app" -- which is s11's sentence again, moved one room
    // along. s11 fixed being lost in the SETUP; this is being lost in the
    // PRODUCT, and the two are the same defect at different depths: every
    // session built a layer and no session ever wrote the first sentence.
    //
    // The explanation existed. It was in the header comment of core/Case.hpp,
    // which is a better description of this program than anything that has
    // ever been on screen, and which only a maintainer reads. Putting it in
    // the window is not documentation -- it is moving the app's own account of
    // itself to the side of the seam where the user is.
    //
    // ON THIS PAGE rather than on Cases, which is Scott's call. The roster
    // answers "who are these companies"; the question standing behind that is
    // "and what is this program", and they belong together. It is also the
    // page the app opens on, so it is the first thing anybody reads.
    //
    // The lede fixes what the app is and why anyone should want it. The
    // second block is the loop, NUMBERED, in the order the work happens --
    // the same idiom the settings window earned in s11, because that idiom is
    // what stopped people getting lost the last time.
    m_roster_lede.set_markup(
        "<span size=\"large\" weight=\"bold\">delr checks whether data brokers "
        "actually removed you.</span>\n"
        "A confirmation email is a claim. delr fetches the live listing page "
        "itself, through your VPN tunnel, and reads what is really there -- "
        "so the answer comes from the page rather than from the company that "
        "profits from the listing.");
    m_roster_lede.set_xalign(0.0f);
    m_roster_lede.set_wrap(true);
    m_roster_lede.set_margin(12);
    m_roster_lede.set_margin_bottom(4);

    // Whose job each step is, said in the step rather than in a preamble. The
    // two things delr deliberately will NOT do are stated as choices with
    // reasons, because an absence with no reason reads as a missing feature
    // and invites someone to add it.
    m_roster_how.set_markup(
        "<b>How you use it</b>\n"
        "<b>1.</b>  You find a listing of yourself on a broker's site, and "
        "paste its address into delr with <b>+</b> above. That becomes a "
        "<i>case</i> -- one listing, not one broker.\n"
        "<b>2.</b>  You send the opt-out yourself, by the method this table "
        "names for that broker.\n"
        "<b>3.</b>  delr fetches the page again -- <b>Check now</b>, on the "
        "Cases page -- and reads it against rules written for that broker.\n"
        "\n"
        "It records <i>listed</i>, <i>not found</i>, or <i>couldn't tell</i>, "
        "and the third never quietly becomes the second. It wants two clean "
        "absences before believing a removal: one is an event, two is a "
        "pattern.\n"
        "\n"
        "delr never searches for you and never sends the opt-out for you. "
        "Both are deliberate -- searching yourself on a broker is itself "
        "something they record, and an opt-out is a legal request that should "
        "come from you.");
    m_roster_how.set_xalign(0.0f);
    m_roster_how.set_wrap(true);
    m_roster_how.set_margin_start(12);
    m_roster_how.set_margin_end(12);
    m_roster_how.set_margin_bottom(12);

    // The count line, which is the only thing that used to be here, now reads
    // as a caption to the table below it instead of as the page's opening
    // statement.
    m_roster_status.set_xalign(0.0f);
    m_roster_status.set_wrap(true);
    m_roster_status.set_margin(8);

    m_roster_scroll.set_child(m_roster_list);
    m_roster_scroll.set_vexpand(true);
    m_roster_page.append(m_roster_lede);
    m_roster_page.append(m_roster_how);
    m_roster_page.append(m_roster_rule);
    m_roster_page.append(m_roster_status);
    m_roster_page.append(m_roster_scroll);
    m_stack.add(m_roster_page, "roster", "Roster");

    // Cases: the per-user caseload. Same shape as the roster page on purpose --
    // status line on top, list below -- because they are the same job with a
    // different table, and two surfaces that behave alike should be built alike.
    //
    // The exposure line sits BETWEEN them rather than in the list, because it
    // answers a different question: the list says "where", the roll-up says
    // "what of yours is out there", and that second sentence is the one worth
    // reading first.
    m_egress_status.set_xalign(0.0f);
    m_egress_status.set_wrap(true);
    m_egress_status.set_margin_start(8);
    m_egress_status.set_margin_end(8);
    m_egress_status.set_margin_top(8);
    m_cases_status.set_xalign(0.0f);
    m_cases_status.set_margin(8);
    m_cases_exposure.set_xalign(0.0f);
    m_cases_exposure.set_margin_start(8);
    m_cases_exposure.set_margin_end(8);
    m_cases_exposure.set_margin_bottom(8);
    m_cases_exposure.set_wrap(true);
    m_cases_maintenance.set_xalign(0.0f);
    m_cases_maintenance.set_wrap(true);
    m_cases_maintenance.set_margin_start(8);
    m_cases_maintenance.set_margin_end(8);
    m_cases_maintenance.set_margin_bottom(8);

    // The check. One button, acting on the selected row, with the last
    // verdict beside it in words -- never a url, never an address, because
    // this line is the one a user screenshots when asking what went wrong.
    m_check_button.set_label("Check now");
    m_check_button.set_tooltip_text(
        "Fetch the selected listing through the tunnel and record what the "
        "page says");
    m_check_button.set_action_name("win.check-now");
    m_check_state.set_xalign(0.0f);
    m_check_state.set_wrap(true);
    m_check_state.set_hexpand(true);
    m_check_row.set_margin_start(8);
    m_check_row.set_margin_end(8);
    m_check_row.set_margin_bottom(8);
    m_compose_button.set_label("Compose request");
    m_compose_button.set_tooltip_text(
        "Draft an opt-out request for the selected listing. delr does not "
        "send it.");
    m_compose_button.set_action_name("win.compose");

    m_check_row.append(m_check_button);
    m_check_row.append(m_compose_button);
    m_check_row.append(m_check_state);

    // Selection exists so the button has something to act on. SINGLE rather
    // than the default: a check is a request that leaves this machine, and
    // "which one" should never be inferred.
    m_cases_list.set_selection_mode(Gtk::SelectionMode::SINGLE);

    m_cases_scroll.set_child(m_cases_list);
    m_cases_scroll.set_vexpand(true);
    m_cases_page.append(m_egress_status);
    m_cases_page.append(m_cases_status);
    m_cases_page.append(m_cases_exposure);
    m_cases_page.append(m_cases_maintenance);
    m_cases_page.append(m_check_row);
    m_cases_page.append(m_cases_scroll);
    m_stack.add(m_cases_page, "cases", "Cases");
}

// ─────────────────────────────────────────────────────────────────────────────
// The profile page -- the person
//
// s13 asked Scott what he thought the app did and he answered in six lines.
// Line one was "build a user profile", and it had never existed. Nor had the
// explanation for line two: he said of the tunnel, "I don't know how it really
// helps but I just thought it wise." Right conclusion, no reason -- which
// means the app never gave him one. Both of those are on this page.
//
// FIVE TEXT BOXES, one term per line, and every judgement about what a term is
// belongs to `core::terms_parse`. The form reads text and hands it over; it
// does not trim, does not deduplicate, does not decide what counts. The
// surface decides nothing.
// ─────────────────────────────────────────────────────────────────────────────

void Shell::TermBox::build(const std::string& label, const std::string& hint) {
    caption.set_markup("<b>" + label + "</b>  <span size=\"small\">" + hint +
                       "</span>");
    caption.set_xalign(0.0f);
    caption.set_wrap(true);
    caption.set_margin_top(8);

    view.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
    view.set_top_margin(4);
    view.set_bottom_margin(4);
    view.set_left_margin(6);
    view.set_right_margin(6);

    // Its own scroller with a fixed height, rather than letting five growing
    // boxes fight the page's scroller. NEVER horizontally: a term that runs
    // off the side is a term the user cannot see they typed.
    scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll.set_size_request(-1, 76);
    scroll.set_child(view);
    scroll.set_has_frame(true);
}

std::string Shell::TermBox::text() const {
    auto buf = view.get_buffer();
    return buf ? std::string(buf->get_text()) : std::string();
}

void Shell::TermBox::set_text(const std::string& s) {
    if (auto buf = view.get_buffer()) buf->set_text(s);
}

void Shell::build_profile_page() {
    m_profile_lede.set_markup(
        "<span size=\"large\" weight=\"bold\">Who delr is looking for.</span>\n"
        "A broker's page is only evidence about <i>you</i> if your own details "
        "are on it. Without this, delr can tell that a page loaded and not "
        "whether the record on it is yours — so it refuses to call anything "
        "removed, which is the honest answer and not a useful one.\n"
        "\n"
        "Type what a listing would <b>print</b>: names you have gone by, places "
        "you have lived, numbers, handles. Old ones matter most — a broker's "
        "record is a decade of accretion, and the address from three moves ago "
        "is often the one that finds you.\n"
        "\n"
        "<b>None of this is sent anywhere by typing it.</b> These are search "
        "terms, kept on this machine, mode 0600. Filing an opt-out is a "
        "separate act against a named broker, and delr will say what it is "
        "about to disclose before it discloses it.");
    m_profile_lede.set_xalign(0.0f);
    m_profile_lede.set_wrap(true);
    m_profile_lede.set_margin(12);
    m_profile_lede.set_margin_bottom(4);

    // Scott's sentence, answered. It is here rather than in the settings
    // window because this is where the cost is visible: these are the terms a
    // check carries, and the check is the leak.
    m_profile_tunnel.set_markup(
        "<b>Why the tunnel</b>\n"
        "Looking yourself up on a broker's site is itself something they "
        "record. Every check hands them a fresh visit from your address, which "
        "is roughly your town, on the exact day you looked — from the one "
        "person on earth most interested in that listing. That is new data "
        "about you, created by the act of checking.\n"
        "The tunnel is why the check is safe: the broker sees the VPN's exit, "
        "not yours. <b>The check is the leak; the tunnel is what makes it "
        "affordable.</b>  Set it up under <i>Tunnel and privacy</i> "
        "(Ctrl+comma) — delr refuses to fetch anything until you have.");
    m_profile_tunnel.set_xalign(0.0f);
    m_profile_tunnel.set_wrap(true);
    m_profile_tunnel.set_margin_start(12);
    m_profile_tunnel.set_margin_end(12);
    m_profile_tunnel.set_margin_bottom(12);

    // ── the form ────────────────────────────────────────────────────────────
    m_profile_name_caption.set_markup(
        "<b>Full name</b>  <span size=\"small\">as a listing would print it</span>");
    m_profile_name_caption.set_xalign(0.0f);
    m_profile_name.set_placeholder_text("John Smith");

    m_profile_aka.build("Also known as",
                        "one per line — maiden name, nicknames, other spellings");
    m_profile_emails.build("Email addresses",
                           "one per line — every address you have used, not just "
                           "the current one");

    m_profile_contact_caption.set_markup(
        "<b>File opt-outs from</b>  <span size=\"small\">must be one of the "
        "addresses above — an inbox you actually read</span>");
    m_profile_contact_caption.set_xalign(0.0f);
    m_profile_contact_caption.set_wrap(true);
    m_profile_contact_caption.set_margin_top(8);
    m_profile_contact.set_placeholder_text("john@example.com");

    m_profile_phones.build("Phone numbers",
                           "one per line — delr matches the punctuated forms a "
                           "page prints, so type it any way");
    m_profile_usernames.build("Usernames",
                              "one per line — handles that are distinctively yours");
    m_profile_places.build("Places you have lived",
                           "one per line — \"Nashville, TN\", a zip, a street. "
                           "This is what tells two people of the same name apart.");

    m_profile_year_caption.set_markup(
        "<b>Birth year</b>  <span size=\"small\">year only, and only because "
        "listings print an age. delr does not want your date of birth.</span>");
    m_profile_year_caption.set_xalign(0.0f);
    m_profile_year_caption.set_wrap(true);
    m_profile_year_caption.set_margin_top(8);
    m_profile_year.set_placeholder_text("1984");
    m_profile_year.set_max_width_chars(8);
    m_profile_year.set_hexpand(false);
    m_profile_year.set_halign(Gtk::Align::START);

    // ── Where you live ──────────────────────────────────────────────────────
    // Not a search term, and the caption says so, because a user who reads
    // this form top to bottom has been answering "what should delr look for"
    // eight times in a row and this box is a different question.
    m_profile_residency_caption.set_markup(
        "<b>Where you live</b>  <span size=\"small\">two-letter state — this is "
        "not a search term. It decides which law delr may invoke on your "
        "behalf, and blank means it invokes none.</span>");
    m_profile_residency_caption.set_xalign(0.0f);
    m_profile_residency_caption.set_wrap(true);
    m_profile_residency_caption.set_margin_top(8);
    m_profile_residency.set_placeholder_text("TN");
    m_profile_residency.set_max_width_chars(8);
    m_profile_residency.set_hexpand(false);
    m_profile_residency.set_halign(Gtk::Align::START);

    m_profile_form.set_margin_start(12);
    m_profile_form.set_margin_end(12);
    m_profile_form.set_margin_bottom(12);
    m_profile_form.append(m_profile_name_caption);
    m_profile_form.append(m_profile_name);
    m_profile_form.append(m_profile_aka.caption);
    m_profile_form.append(m_profile_aka.scroll);
    m_profile_form.append(m_profile_emails.caption);
    m_profile_form.append(m_profile_emails.scroll);
    m_profile_form.append(m_profile_contact_caption);
    m_profile_form.append(m_profile_contact);
    m_profile_form.append(m_profile_phones.caption);
    m_profile_form.append(m_profile_phones.scroll);
    m_profile_form.append(m_profile_usernames.caption);
    m_profile_form.append(m_profile_usernames.scroll);
    m_profile_form.append(m_profile_places.caption);
    m_profile_form.append(m_profile_places.scroll);
    m_profile_form.append(m_profile_year_caption);
    m_profile_form.append(m_profile_year);
    m_profile_form.append(m_profile_residency_caption);
    m_profile_form.append(m_profile_residency);

    m_profile_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_profile_scroll.set_child(m_profile_form);
    m_profile_scroll.set_vexpand(true);

    // Save is explicit. A form that writes as you type would put a
    // half-entered address on disk and, worse, would make "I changed my mind"
    // unexpressible: closing without saving has to be a thing a person can do
    // with the most sensitive file this program owns.
    m_profile_save.set_label("Save profile");
    m_profile_save.set_tooltip_text(
        "Write the profile to this machine, mode 0600 (Ctrl+S)");
    m_profile_save.set_action_name("win.save-profile");
    m_profile_status.set_xalign(0.0f);
    m_profile_status.set_wrap(true);
    m_profile_status.set_hexpand(true);
    m_profile_actions.set_margin(12);
    m_profile_actions.append(m_profile_save);
    m_profile_actions.append(m_profile_status);

    m_profile_page.append(m_profile_lede);
    m_profile_page.append(m_profile_tunnel);
    m_profile_page.append(m_profile_rule);
    m_profile_page.append(m_profile_scroll);
    m_profile_page.append(m_profile_actions);
    m_stack.add(m_profile_page, "profile", "You");
}

// Sections, not one flat list. A menu that runs "add, settings, reload, reload,
// reload, dump, quit" in a single column makes a developer's diagnostic look
// like a thing a user is supposed to press. The dividers say which items are
// the app and which are the workbench.
Glib::RefPtr<Gio::Menu> Shell::build_menu() {
    auto menu = Gio::Menu::create();

    auto doing = Gio::Menu::create();
    doing->append("Add case...",         "win.add-case");
    doing->append("Check selected case", "win.check-now");
    menu->append_section({}, doing);

    auto config = Gio::Menu::create();
    config->append("Save profile",          "win.save-profile");
    config->append("Tunnel and privacy...", "win.egress");
    menu->append_section({}, config);

    // Reloads read the tables back off disk. They are here because the
    // tables are editable files by design and someone will edit one.
    auto reload = Gio::Menu::create();
    reload->append("Reload cases",      "win.reload-cases");
    reload->append("Reload roster",     "win.reload-roster");
    reload->append("Reload page rules", "win.reload-rules");
    reload->append("Reload statutes",   "win.reload-statutes");
    reload->append("Reload profile",    "win.reload-profile");
    menu->append_section({}, reload);

    // The workbench, alone in its own section and last but for Quit. It prints
    // the widget registry to the log and means nothing to anyone who is not
    // debugging this window.
    auto tools = Gio::Menu::create();
    tools->append("Dump widget registry", "win.dump-registry");
    menu->append_section({}, tools);

    auto leaving = Gio::Menu::create();
    leaving->append("Quit", "win.quit");
    menu->append_section({}, leaving);
    return menu;
}

}  // namespace delr
