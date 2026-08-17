#pragma once
#include <gtkmm/application.h>

// App -- the Gtk::Application. Thin: stands up logging, builds the Shell on
// activate (CANON: thin bootstrap -- main -> App::create() -> Shell).
namespace delr {

class Shell;

class App : public Gtk::Application {
public:
    static Glib::RefPtr<App> create();

protected:
    App();
    void on_activate() override;

private:
    Shell* m_shell = nullptr;   // owned by the application via add_window
};

}  // namespace delr
