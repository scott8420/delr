#pragma once
#include "widgets/Widgets.hpp"
#include "core/Broker.hpp"
#include "core/Case.hpp"
#include "core/Intake.hpp"

#include <gtkmm/window.h>
#include <gtkmm/stringlist.h>

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// AddCaseDialog -- the paste-a-URL surface.
//
// Discovery is the user's job and this window is where the result of it lands:
// they find the listing in their own browser, paste the address here, and the
// app turns it into a case it can verify forever after. Until this existed the
// only way to create a case was to hand-edit JSON, which made the whole feature
// unreachable to anyone who is not us.
//
// THE DIALOG DECIDES NOTHING. Every judgment -- is this a URL, whose site is
// it, do we already have this listing, is this a relist, what id -- belongs to
// core/Intake, which is pure and tested headless. This file reads a report and
// arranges labels. When you find yourself about to write a rule here, it goes
// in Intake.cpp instead and comes back as a field on the report.
//
// Lifetime: HIDE ON CLOSE (the Cairn pattern). The window is built once, as a
// member of the Shell, and closing it hides it rather than destroying it, so
// re-opening is instant and there is no half-torn-down window to hand a signal
// to. The corollary that matters HERE and not in Cairn: a hidden window is
// still holding whatever was typed into it, and what gets typed into this one
// is a URL with a person's name in the path. So hiding CLEARS -- the entry, the
// note, the field ticks, and the caseload copy taken at open. A privacy tool
// that leaves a dossier in a hidden widget has defeated itself the same way one
// that leaves it in a log has.
// ─────────────────────────────────────────────────────────────────────────────
namespace delr {

class AddCaseDialog : public Gtk::Window {
public:
    AddCaseDialog();
    ~AddCaseDialog() override;

    // Open against a SNAPSHOT of the two tables. Copies, not references: the
    // report hands back pointers into the roster and caseload it was given, and
    // a reload behind our back would reallocate the Shell's vectors underneath
    // them. The snapshot is also why the Shell re-checks the id at commit time.
    void open(Gtk::Window& parent, const core::Roster& roster,
              const core::Caseload& caseload, const std::string& today);

    // Emitted with the case to commit. The dialog never writes a file -- the
    // Shell owns the path, the pump and the repaint (the same seam that keeps
    // path resolution and the clock on the UI side and out of the core).
    sigc::signal<void(core::Case)>& signal_committed() { return m_committed; }

private:
    void build();                    // widgets and layout
    void bind();                     // signals
    void refresh();                  // re-inspect the paste, repaint the verdict
    void on_add();
    void on_hidden();                // the clearing described above

    // The broker the case will be filed under: the roster match when there is
    // one, or whatever the user chose. A dropdown that can only agree with the
    // matcher would make an unrecognised host a dead end, and the roster will
    // always trail the web.
    const core::Broker* chosen_broker() const;
    void select_broker(const std::string& id);   // programmatic, guarded

    widgets::Box   m_root, m_broker_row, m_buttons;
    widgets::Label m_prompt, m_verdict, m_broker_label, m_exposes_label;
    widgets::Entry m_url, m_note;
    widgets::DropDown m_broker;
    widgets::FlowBox  m_exposes;
    widgets::Button   m_cancel, m_add;

    Glib::RefPtr<Gtk::StringList> m_broker_names;
    std::vector<std::string>      m_broker_ids;      // parallel to the model
    std::vector<std::pair<core::Field, Gtk::CheckButton*>> m_field_boxes;

    core::Roster   m_roster;      // snapshots, cleared on hide
    core::Caseload m_caseload;
    std::string    m_today;

    // True once the user has touched the dropdown themselves. After that the
    // matcher stops overriding their choice on every keystroke -- an override
    // that fights the user is worse than no help at all.
    bool m_broker_pinned = false;
    bool m_setting       = false;   // guards programmatic selection

    sigc::signal<void(core::Case)> m_committed;
};

}  // namespace delr
