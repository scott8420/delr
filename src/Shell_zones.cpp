// Zones -- construction of named regions. No behaviour here.
#include "Shell.hpp"

#include <gtkmm/headerbar.h>
#include <gtkmm/popovermenu.h>

namespace delr {

void Shell::build_shell() {
    set_title("delr");
    set_default_size(900, 600);

    auto* header = Gtk::make_managed<Gtk::HeaderBar>();
    m_menu_button.set_icon_name("open-menu-symbolic");
    m_menu_button.set_menu_model(build_menu());
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

    // Cases: the per-user caseload. Deliberately empty -- the model isn't
    // built yet, and a page that pretends to work would be the un-run
    // reference CANON warns about.
    m_cases_label.set_text("No caseload model yet.\n"
                           "Cases arrive once discovery is a human step and "
                           "verification is the machine's.");
    m_cases_label.set_justify(Gtk::Justification::CENTER);
    m_cases_label.set_vexpand(true);
    m_cases_page.append(m_cases_label);
    m_stack.add(m_cases_page, "cases", "Cases");
}

Glib::RefPtr<Gio::Menu> Shell::build_menu() {
    auto menu = Gio::Menu::create();
    menu->append("Reload roster", "win.reload-roster");
    menu->append("Dump registry", "win.dump-registry");
    return menu;
}

}  // namespace delr
