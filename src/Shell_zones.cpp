// Zones -- construction of named regions. No behaviour here.
#include "Shell.hpp"

#include <gtkmm/headerbar.h>
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
    // Roster: the broker table, read from JSON.
    m_roster_status.set_xalign(0.0f);
    m_roster_status.set_margin(8);
    m_roster_scroll.set_child(m_roster_list);
    m_roster_scroll.set_vexpand(true);
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
    m_check_row.append(m_check_button);
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

Glib::RefPtr<Gio::Menu> Shell::build_menu() {
    auto menu = Gio::Menu::create();
    menu->append("Add case...",   "win.add-case");
    menu->append("Tunnel and privacy...", "win.egress");
    menu->append("Reload roster", "win.reload-roster");
    menu->append("Reload cases",  "win.reload-cases");
    menu->append("Reload page rules", "win.reload-rules");
    menu->append("Dump registry", "win.dump-registry");
    return menu;
}

}  // namespace delr
