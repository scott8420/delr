#include "App.hpp"
#include "Shell.hpp"
#include "Log.hpp"

namespace delr {

App::App() : Gtk::Application("org.delr.App") {}

Glib::RefPtr<App> App::create() {
    log::init();
    if (auto lg = log::get(log::Area::App)) lg->info("delr starting");
    return Glib::make_refptr_for_instance<App>(new App());
}

void App::on_activate() {
    if (!m_shell) {
        m_shell = new Shell();
        add_window(*m_shell);
        m_shell->signal_hide().connect([this] { delete m_shell; m_shell = nullptr; });
        m_shell->build_ui();
    }
    m_shell->present();
}

}  // namespace delr
