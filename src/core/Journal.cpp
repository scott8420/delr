#include "core/Journal.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <system_error>

using json = nlohmann::json;

namespace delr::core {

// ── Enum table ───────────────────────────────────────────────────────────────
// Named, not bare integers, and round-tripped in the selftest. These strings
// are on disk forever in a file nothing rewrites, so a rename here does not
// change the future -- it changes what the past appears to say.

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Opened:   return "opened";
        case Kind::Checked:  return "checked";
        case Kind::Declined: return "declined";
        case Kind::Changed:  return "changed";
        case Kind::Other:    return "other";
    }
    return "other";
}

Kind kind_from(const std::string& s) {
    if (s == "opened")   return Kind::Opened;
    if (s == "checked")  return Kind::Checked;
    if (s == "declined") return Kind::Declined;
    if (s == "changed")  return Kind::Changed;
    return Kind::Other;   // and the caller keeps the label. See the header.
}

// ── Builders ─────────────────────────────────────────────────────────────────
// Every one of these reads the case rather than taking the values loose. A
// second opinion about what just happened is a second opinion that can be
// wrong, and this is the copy that does not get corrected later.

namespace {
Entry base_of(const Case& k, Kind kind, const std::string& today) {
    Entry e;
    e.date      = today;
    e.case_id   = k.id;
    e.broker_id = k.broker_id;
    e.kind      = kind;
    return e;
}
}  // namespace

Entry entry_opened(const Case& k, const std::string& today) {
    return base_of(k, Kind::Opened, today);
}

Entry entry_checked(const Case& k, const std::string& today) {
    Entry e = base_of(k, Kind::Checked, today);
    e.outcome = k.outcome;
    e.reason  = k.reason;
    return e;
}

Entry entry_declined(const Case& k, Reason r, const std::string& today) {
    Entry e = base_of(k, Kind::Declined, today);
    e.reason = r;
    // No outcome, deliberately. `Outcome::Never` here does not mean "no check
    // has run yet" the way it does on a case -- it means this row is not about
    // what a fetch saw, because there was no fetch. Writing Indeterminate
    // would make a decline look like a failed look, and the whole point of the
    // kind is that nothing was looked at.
    return e;
}

Entry entry_changed(const Case& k, Status from, Status to,
                    const std::string& today, const std::string& other_id) {
    Entry e = base_of(k, Kind::Changed, today);
    e.from       = from;
    e.to         = to;
    e.provenance = k.provenance;
    e.other_id   = other_id;
    return e;
}

std::string log_ref(const Entry& e) {
    return "entry:" + std::to_string(e.seq) + "/" + kind_name(e.kind) +
           "@" + e.case_id;
}

// ── Reading ──────────────────────────────────────────────────────────────────

std::vector<const Entry*> journal_for_case(const Journal& j,
                                           const std::string& case_id) {
    std::vector<const Entry*> out;
    if (case_id.empty()) return out;
    for (const auto& e : j)
        if (e.case_id == case_id) out.push_back(&e);
    return out;
}

const Entry* journal_last(const Journal& j, const std::string& case_id, Kind k) {
    if (case_id.empty() || k == Kind::Other) return nullptr;
    const Entry* found = nullptr;
    for (const auto& e : j)
        if (e.case_id == case_id && e.kind == k) found = &e;
    return found;
}

const Entry* journal_first(const Journal& j, const std::string& case_id, Kind k) {
    if (case_id.empty() || k == Kind::Other) return nullptr;
    for (const auto& e : j)
        if (e.case_id == case_id && e.kind == k) return &e;
    return nullptr;
}

std::vector<const Entry*> journal_since(const Journal& j, const std::string& iso) {
    std::vector<const Entry*> out;
    // A malformed filter that silently means "no filter" is how a report ends
    // up claiming a decade of activity happened last week.
    if (!date_valid(iso)) return out;
    for (const auto& e : j)
        if (date_valid(e.date) && date_compare(e.date, iso) >= 0) out.push_back(&e);
    return out;
}

// ── Walls ────────────────────────────────────────────────────────────────────

bool reason_is_wall(Reason r) {
    switch (r) {
        case Reason::Blocked:       // unattributed -- still a wall
        case Reason::EgressBlocked:
        case Reason::ClientBlocked:
        case Reason::NoListingPage:
            return true;
        default:
            return false;
    }
}

std::string journal_walled_since(const Journal& j, const std::string& case_id) {
    if (case_id.empty()) return {};

    // Walk backwards. The run we want is the CURRENT one, so the first thing
    // that breaks it going up the file ends the search -- and a wall further
    // back, on the far side of a clean fetch, belongs to a different run and
    // is not this one's start date.
    std::string start;
    for (auto it = j.rbegin(); it != j.rend(); ++it) {
        const Entry& e = *it;
        if (e.case_id != case_id) continue;

        // Our own outage is not evidence about the broker: it neither extends
        // the run nor breaks it. Same reasoning as `apply_egress_refusal`
        // leaving the failure streak alone.
        if (e.kind == Kind::Declined) continue;

        // Nor is anything that is not a look. A status change in the middle of
        // a walled stretch does not mean the wall came down.
        if (e.kind != Kind::Checked) continue;

        if (e.outcome == Outcome::Indeterminate && reason_is_wall(e.reason)) {
            start = e.date;      // keep walking; an earlier one may extend it
            continue;
        }

        // A check that was not a wall -- clean, or one of our own bugs. Either
        // way we got a look at something, and the run ends here.
        break;
    }
    return start;
}

// ── Tally ────────────────────────────────────────────────────────────────────

Tally journal_tally(const Journal& j, const std::string& case_id) {
    Tally t;
    for (const auto& e : j) {
        if (!case_id.empty() && e.case_id != case_id) continue;

        if (e.kind == Kind::Declined) { ++t.declined; ++t.ours; continue; }
        if (e.kind != Kind::Checked) continue;

        ++t.checked;
        if (e.outcome == Outcome::Listed || e.outcome == Outcome::NotFound) {
            ++t.clean;
        } else if (e.outcome == Outcome::Indeterminate) {
            if (reason_is_wall(e.reason)) ++t.walled;
            // NoRule, PageUnreadable and NoTunnel are ours, and the honest
            // denominator has to say so: a rule nobody wrote is not a broker
            // being difficult.
            else if (e.reason == Reason::NoRule ||
                     e.reason == Reason::PageUnreadable ||
                     e.reason == Reason::NoTunnel) ++t.ours;
        }
    }
    return t;
}

// ── Validation ───────────────────────────────────────────────────────────────

std::vector<std::string> journal_validate(const Journal& j) {
    std::vector<std::string> problems;

    std::int64_t prev_seq  = 0;
    std::string  prev_date;

    for (const auto& e : j) {
        const std::string ref = log_ref(e);

        if (e.seq <= 0)
            problems.push_back(ref + ": non-positive seq");
        else if (e.seq <= prev_seq)
            problems.push_back(ref + ": seq does not increase");
        if (e.seq > prev_seq) prev_seq = e.seq;

        if (e.date.empty())
            problems.push_back(ref + ": missing date");
        else if (!date_valid(e.date))
            problems.push_back(ref + ": malformed date");
        else {
            // Reported, never refused. A clock that went backwards is worth
            // knowing about; it is not worth discarding the evidence over.
            if (!prev_date.empty() && date_compare(e.date, prev_date) < 0)
                problems.push_back(ref + ": date goes backwards");
            prev_date = e.date;
        }

        if (e.case_id.empty())
            problems.push_back(ref + ": missing case_id");

        switch (e.kind) {
            case Kind::Checked:
                if (e.outcome == Outcome::Never)
                    problems.push_back(ref + ": checked without an outcome");
                // The invariant `Case` guards, guarded again here: the journal
                // is a second copy of the same claim and the two must not be
                // able to disagree about whether a failure had a cause.
                if (e.outcome == Outcome::Indeterminate && e.reason == Reason::None)
                    problems.push_back(ref + ": indeterminate without a reason");
                if (e.outcome != Outcome::Indeterminate && e.reason != Reason::None)
                    problems.push_back(ref + ": reason on a non-indeterminate outcome");
                break;

            case Kind::Declined:
                if (e.reason == Reason::None)
                    problems.push_back(ref + ": declined without a reason");
                if (e.outcome != Outcome::Never)
                    problems.push_back(ref + ": declined carries an outcome");
                break;

            case Kind::Changed:
                if (e.from == e.to)
                    problems.push_back(ref + ": changed to what it already was");
                break;

            case Kind::Opened:
                break;

            case Kind::Other:
                // Unrecoverable only if the label is gone too -- then the row
                // says nothing at all and cannot be handed forward.
                if (e.kind_raw.empty())
                    problems.push_back(ref + ": unknown kind with no label");
                break;
        }
    }
    return problems;
}

// ── Pump ─────────────────────────────────────────────────────────────────────

namespace {

Entry decode(const json& o) {
    Entry e;
    e.seq       = o.value("seq", static_cast<std::int64_t>(0));
    e.date      = o.value("date", "");
    e.case_id   = o.value("case_id", "");
    e.broker_id = o.value("broker_id", "");

    const std::string raw = o.value("kind", "other");
    e.kind = kind_from(raw);
    // The label is kept ONLY when we did not understand it. Keeping it always
    // would give encode two sources for one field, and the two would drift the
    // first time somebody renamed a kind.
    if (e.kind == Kind::Other) e.kind_raw = raw;

    e.outcome    = outcome_from(o.value("outcome", "never"));
    e.reason     = reason_from(o.value("reason", "none"));
    e.from       = status_from(o.value("from", "unknown"));
    e.to         = status_from(o.value("to", "unknown"));
    e.provenance = provenance_from(o.value("provenance", "none"));
    e.other_id   = o.value("other_id", "");
    return e;
}

json encode(const Entry& e) {
    json o;
    o["seq"]  = e.seq;
    o["date"] = e.date;
    o["case_id"]   = e.case_id;
    o["broker_id"] = e.broker_id;
    // An `Other` entry goes back out under the label it came in with. This is
    // the whole forward-tolerance contract: a binary that does not understand
    // a row must hand it back unchanged rather than flatten it to "other" and
    // destroy somebody's proof that they filed.
    o["kind"] = (e.kind == Kind::Other && !e.kind_raw.empty())
                    ? e.kind_raw
                    : std::string(kind_name(e.kind));
    o["outcome"]    = outcome_name(e.outcome);
    o["reason"]     = reason_name(e.reason);
    o["from"]       = status_name(e.from);
    o["to"]         = status_name(e.to);
    o["provenance"] = provenance_name(e.provenance);
    o["other_id"]   = e.other_id;
    return o;
}

}  // namespace

Journal journal_load(const std::string& file, std::string* error, int* skipped) {
    if (error)   error->clear();
    if (skipped) *skipped = 0;

    std::ifstream in(file);
    if (!in) return {};   // first-run tolerance: absent is empty, not an error

    Journal out;
    std::string line;
    while (std::getline(in, line)) {
        // Blank lines are not damage. A torn tail healed by a leading newline
        // on the next append leaves exactly one, and counting it as a skipped
        // entry would report corruption that is not there.
        bool blank = true;
        for (char c : line) if (c != ' ' && c != '\t' && c != '\r') { blank = false; break; }
        if (blank) continue;

        json o;
        try {
            o = json::parse(line);
        } catch (const std::exception&) {
            if (skipped) ++*skipped;   // one bad line, not one bad file
            continue;
        }
        if (!o.is_object()) { if (skipped) ++*skipped; continue; }
        out.push_back(decode(o));
    }

    // `bad()` is a read error -- the file exists and the bytes would not come
    // back. `fail()` alone is just the eof that ends every getline loop, and
    // reporting it would make every healthy read look broken.
    if (in.bad() && error) *error = "read failed";
    return out;
}

std::int64_t journal_next_seq(const Journal& j) {
    std::int64_t hi = 0;
    for (const auto& e : j) if (e.seq > hi) hi = e.seq;
    return hi + 1;
}

bool journal_append(const std::string& file, Entry& e, std::int64_t next_seq) {
    e.seq = next_seq;

    // Mode 0600 before the first byte, same as the egress policy and the
    // profile. Re-applied every append rather than only at creation: a file
    // that got made with the wrong mode once stays wrong forever otherwise.
    {
        std::ofstream create(file, std::ios::app);
        if (!create) return false;
    }
    std::error_code pec;
    std::filesystem::permissions(
        file,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, pec);

    // Heal a torn tail. A process killed mid-append leaves a line with no
    // newline; writing straight onto it would fuse a good entry to a broken
    // one and cost two rows instead of one.
    bool needs_nl = false;
    {
        std::error_code sec;
        const auto sz = std::filesystem::file_size(file, sec);
        if (!sec && sz > 0) {
            std::ifstream tail(file, std::ios::binary);
            if (tail) {
                tail.seekg(-1, std::ios::end);
                char last = '\n';
                tail.get(last);
                needs_nl = (last != '\n');
            }
        }
    }

    std::ofstream out(file, std::ios::app);
    if (!out) return false;
    if (needs_nl) out << "\n";
    out << encode(e).dump() << "\n";
    out.flush();
    return out.good();
}

bool journal_record(const std::string& file, Entry& e) {
    // Reload rather than trusting a cached sequence: two processes -- the app
    // and a `--run-due` timer firing at 3am -- can hold this file, and a seq
    // taken from memory five minutes ago would collide. The read is one pass
    // over a small append-only file and it is the cheap half of the write.
    const Journal j = journal_load(file);
    return journal_append(file, e, journal_next_seq(j));
}

}  // namespace delr::core
