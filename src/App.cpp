#include "App.hpp"
#include "Shell.hpp"
#include "Appearance.hpp"
#include "Log.hpp"

namespace delr {

App::App() : Gtk::Application("org.delr.App") {}

Glib::RefPtr<App> App::create() {
    log::init();
    if (auto lg = log::get(log::Area::App)) lg->info("delr starting");
    return Glib::make_refptr_for_instance<App>(new App());
}

void App::on_activate() {
    // ── THE DESKTOP'S LIGHT/DARK PREFERENCE, BEFORE ANY WINDOW EXISTS ───────
    // GTK4 does not follow it on its own and delr does not link libadwaita, so
    // the app has to ask (see Appearance.hpp). Asking HERE, ahead of
    // `new Shell()`, is the difference between starting dark and starting
    // light and repainting -- a first frame in the wrong colour is a flash the
    // user sees every launch.
    //
    // Inside the first-activation block, NOT before it: follow_system()
    // subscribes to the portal's change signal, so calling it on a second
    // activation would stack a second subscription (and a second proxy) for no
    // gain. One call keeps following forever, which is the whole point of the
    // subscription.
    if (!m_shell) {
        appearance::follow_system();

        m_shell = new Shell();
        add_window(*m_shell);
        m_shell->signal_hide().connect([this] { delete m_shell; m_shell = nullptr; });
        m_shell->build_ui();
    }
    m_shell->present();
}

}  // namespace delr
