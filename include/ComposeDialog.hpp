#pragma once
#include "widgets/Widgets.hpp"
#include "core/Broker.hpp"
#include "core/Case.hpp"
#include "core/Compose.hpp"
#include "core/Profile.hpp"
#include "core/Statute.hpp"

#include <gtkmm/window.h>

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// ComposeDialog -- the request, on screen, with no Send button.
//
// ── THE MISSING BUTTON IS THE FEATURE ───────────────────────────────────────
//
// Every other window in this program ends in an act delr performs: a check
// fetches, a save writes, a setting persists. This one ends in an act the USER
// performs, somewhere else, in a program that is not this one. See
// `core/Compose`'s header for the three reasons; the one that shows here is
// the second: pressing send is the moment a person reads what is about to
// leave, and this window exists to be that moment rather than to skip it.
//
// So the window says so out loud instead of leaving a gap where a button
// should be. An absence explains nothing, and a user who cannot find Send
// concludes the feature is broken.
//
// ── THE DIALOG DECIDES NOTHING ──────────────────────────────────────────────
//
// Same rule as AddCaseDialog. What the request says, what channel it would go
// out on, which cautions apply and whether it can go at all -- all of it is
// `core::compose_request`, pure and checked headless. This file arranges
// labels around a `core::Request` and offers a clipboard. When a rule wants
// writing here, it goes in Compose.cpp and comes back as a field.
//
// ── The body is EDITABLE, and that is not a contradiction ───────────────────
//
// It is the user's letter. delr drafts it; a person who wants to add a
// sentence is entitled to, and an uneditable box would be the app pretending
// its wording is the only correct wording. What gets copied is what is in the
// box, edits included -- there is no second, secret version.
//
// The cost is that toggling a disclosure redraws the box and discards edits,
// which the window says above it rather than letting a user discover.
//
// ── Lifetime: hide on close, and hiding CLEARS ──────────────────────────────
//
// The Cairn pattern, with AddCaseDialog's corollary and more force behind it.
// A hidden AddCaseDialog holds a URL. A hidden ComposeDialog holds the most
// PII-dense string this program ever assembles -- name, jurisdiction, listing
// and reply address in one block of text -- so hiding drops the body, the
// subject, the address, and the three snapshots taken at open.
//
// Nothing here is ever written to disk. `core/Journal` records that a request
// was filed and through which channel; it does not record what it said, and
// this window is not a back door to that rule.
// ─────────────────────────────────────────────────────────────────────────────
namespace delr {

class ComposeDialog : public Gtk::Window {
public:
    ComposeDialog();
    ~ComposeDialog() override;

    // Open against SNAPSHOTS, for AddCaseDialog's reason: the Shell's tables
    // can be reloaded from the menu while this window is up, and a reference
    // into a vector that has been reassigned is the bug that only appears on
    // somebody else's machine. `law` may be null, and usually is.
    void open(Gtk::Window& parent, const core::Profile& p, const core::Broker& b,
              const core::Case& k, const core::Statute* law,
              const std::string& today);

    // Emitted when the user says they sent it, with the channel they used.
    // The dialog never writes a file: the Shell owns the caseload, the journal
    // and the repaint, exactly as it does for a committed case.
    //
    // The signal carries the user's CLAIM. delr did not send anything and
    // cannot witness a send; what the journal will record is that a person
    // said so, on a date, through a channel.
    // Carries the case id as well as the channel: the Shell finds the row by
    // id rather than by an index taken when the window opened, because the
    // caseload can be reloaded from the menu while this is up. Same reasoning
    // as `on_check_done` re-finding its case.
    sigc::signal<void(std::string, core::Method)>& signal_filed() { return m_filed; }

private:
    void build();
    void bind();
    void recompose();          // re-run core::compose_request and repaint
    void on_copy();
    void on_filed();
    void on_hidden();

    std::string body_text() const;   // what is in the box, edits included

    widgets::Box   m_root, m_options_row, m_buttons;
    widgets::Label m_prompt, m_options_label, m_edit_note, m_no_send;
    // Everything the app WORKED OUT rather than printed. Copyable by
    // construction -- s16's rule, and this is the window where it matters most.
    widgets::Reported m_channel_line, m_cautions, m_standing;
    widgets::TextView       m_body;
    widgets::ScrolledWindow m_body_scroll;
    widgets::CheckButton m_aliases, m_places, m_phones, m_emails;
    widgets::Button m_close, m_copy, m_sent;

    core::Profile m_profile;      // snapshots, cleared on hide
    core::Broker  m_broker;
    core::Case    m_case;
    core::Statute m_law;
    bool          m_has_law = false;
    std::string   m_today;

    core::Request m_request;
    bool m_setting = false;       // guards programmatic toggles

    sigc::signal<void(std::string, core::Method)> m_filed;
};

}  // namespace delr
