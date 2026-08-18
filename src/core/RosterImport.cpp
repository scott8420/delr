#include "core/RosterImport.hpp"

#include "core/Intake.hpp"   // url_host -- the matcher's normal form

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace delr::core {
namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    const auto sp = [](unsigned char c) { return std::isspace(c) != 0; };
    while (a < b && sp(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && sp(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Header cells in this file are sentences -- some wrap across lines in the
// source spreadsheet, so they arrive with embedded newlines and runs of
// spaces. Compare on a squashed, lower-cased form or the matches are luck.
//
// ANY byte that is not printable ASCII counts as a gap, and that clause is
// load-bearing rather than defensive. The 2025 file's very first header reads
// "Data broker name:" and contains a UTF-8 NON-BREAKING SPACE between the two
// words -- C2 A0, invisible in every editor and every terminal, which
// std::isspace does not recognise because it is not one byte. Without this the
// importer refuses the real registry and tells the user the state's own export
// "does not look like the registry", which is a bug that would have survived
// any amount of staring at the file.
//
// The same clause absorbs curly apostrophes ("consumers' precise
// geolocation"), en dashes, and whatever else a spreadsheet inserts next year.
// This is a normal form for MATCHING HEADERS and is never applied to a data
// cell -- squashing those would corrupt names.
std::string squash(const std::string& s) {
    std::string out;
    bool gap = false;
    for (unsigned char c : s) {
        if (c < 0x20 || c >= 0x7F || std::isspace(c)) { gap = !out.empty(); continue; }
        if (gap) { out += ' '; gap = false; }
        out += static_cast<char>(std::tolower(c));
    }
    return out;
}

bool yes(const std::string& s) { return lower(trim(s)) == "yes"; }

std::string cell(const CsvRow& r, int i) {
    if (i < 0 || static_cast<std::size_t>(i) >= r.size()) return {};
    return trim(r[i]);
}

// ── Column identification ────────────────────────────────────────────────────
// By header TEXT, never by index. The registry is refiled annually by an
// agency that has already changed its form once, and a column inserted at
// position 3 would silently shift every field after it -- an importer that
// reads phone numbers into the email field and reports 543 successes. Text
// matching fails loudly instead: the column is not found and the import
// refuses.
struct Columns {
    int name = -1, site = -1, email = -1, rights = -1, fcra = -1, geo = -1;
    bool usable() const { return name >= 0 && site >= 0 && email >= 0; }
};

Columns map_columns(const CsvRow& header) {
    Columns c;
    for (std::size_t i = 0; i < header.size(); ++i) {
        const std::string h = squash(header[i]);
        const int idx = static_cast<int>(i);

        if (c.name < 0 && h.rfind("data broker name", 0) == 0) c.name = idx;

        // Two headers contain "primary website": the registrant's own site,
        // and the page describing how to exercise CCPA rights. The second one
        // is a possessive -- "data broker's" -- and says so at length. Anchor
        // on the start of each so they cannot be confused.
        if (c.site < 0 && h.rfind("data broker primary website", 0) == 0) c.site = idx;
        if (c.rights < 0 && h.find("exercise their ca consumer privacy act rights") !=
                                std::string::npos)
            c.rights = idx;

        if (c.email < 0 && h.find("primary contact email") != std::string::npos)
            c.email = idx;

        // Five headers name the FCRA; four of them begin "if the data broker"
        // and are follow-up detail. The declaration is the one that begins
        // with the statement.
        if (c.fcra < 0 &&
            h.rfind("the data broker or any of its subsidiaries is regulated by the "
                    "federal fair credit", 0) == 0)
            c.fcra = idx;

        if (c.geo < 0 && h.find("precise geolocation") != std::string::npos) c.geo = idx;
    }
    return c;
}

// The header row is not row 0. The file opens with a band of internal
// annotations for the agency's own website ("If not answered by DB, do not
// surface"), so the header is found rather than assumed.
int find_header(const CsvTable& t) {
    const int limit = static_cast<int>(std::min<std::size_t>(t.size(), 10));
    for (int r = 0; r < limit; ++r)
        if (map_columns(t[r]).usable()) return r;
    return -1;
}

std::string slugify(const std::string& s, std::size_t cap) {
    std::string out;
    bool dash = false;
    for (unsigned char c : s) {
        if (std::isalnum(c)) {
            if (dash && !out.empty()) out += '-';
            dash = false;
            out += static_cast<char>(std::tolower(c));
            if (out.size() >= cap) break;
        } else {
            dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

}  // namespace

// ── CSV ──────────────────────────────────────────────────────────────────────

CsvTable csv_parse(const std::string& text) {
    CsvTable table;
    CsvRow   row;
    std::string field;
    bool quoted = false;
    bool any    = false;   // this row has seen a character or a delimiter

    std::size_t i = 0;
    // A UTF-8 BOM ahead of the first cell. Left in place it becomes part of
    // the first header's text and every header match against that column
    // fails -- which is how an importer ends up reporting "no name column" on
    // a file that plainly has one.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        i = 3;

    const auto end_field = [&]() { row.push_back(field); field.clear(); any = true; };
    const auto end_row   = [&]() {
        end_field();
        table.push_back(row);
        row.clear();
        any = false;
    };

    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"') {
                // "" inside a quoted field is one literal quote.
                if (i + 1 < text.size() && text[i + 1] == '"') { field += '"'; ++i; }
                else quoted = false;
            } else {
                field += c;   // newlines inside quotes belong to the field
            }
            continue;
        }
        if (c == '"')      { quoted = true; any = true; }
        else if (c == ',') { end_field(); }
        else if (c == '\r'){ /* CRLF: the \n does the work */ }
        else if (c == '\n'){ end_row(); }
        else               { field += c; any = true; }
    }
    // A final row with no trailing newline still counts; a trailing newline
    // does not invent an empty row.
    if (any || !field.empty()) end_row();
    return table;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

std::string import_host_of(const std::string& url) { return url_host(url); }

std::vector<std::string> split_urls(const std::string& cell_text) {
    std::vector<std::string> out;
    std::string cur;
    const auto flush = [&]() {
        const std::string t = trim(cur);
        cur.clear();
        if (!t.empty()) out.push_back(t);
    };
    // Semicolons and whitespace, both. Registrants separate their domains with
    // "; ", with a bare newline, and in one case with nothing at all -- and a
    // URL cannot contain whitespace, so splitting on it is free.
    for (unsigned char c : cell_text) {
        if (c == ';' || c == ',' || std::isspace(c)) flush();
        else cur += static_cast<char>(c);
    }
    flush();
    return out;
}

std::string mint_id(const std::string& host, const std::string& name,
                    const std::vector<std::string>& taken) {
    // The last label is a TLD and identifies nobody: "explorium.ai" and
    // "explorium.com" are one company far more often than two.
    std::string stem = host;
    const std::size_t dot = stem.rfind('.');
    if (dot != std::string::npos && dot > 0) stem = stem.substr(0, dot);

    std::string base = slugify(stem, 40);
    if (base.empty()) base = slugify(name, 40);
    if (base.empty()) base = "broker";

    const auto free_of = [&](const std::string& cand) {
        return std::find(taken.begin(), taken.end(), cand) == taken.end();
    };
    if (free_of(base)) return base;

    // Eight registrants share transunion.com and four share deepsync.com --
    // separate legal entities filing separately, which the roster must keep
    // separate because they receive requests separately. Their NAMES differ,
    // so the name is the first disambiguator and it produces something a human
    // can read in a log.
    const std::string tail = slugify(name, 32);
    if (!tail.empty() && tail != base) {
        const std::string cand = base + "-" + tail;
        if (free_of(cand)) return cand;
    }
    // And when the names are identical too -- four rows in the 2025 file are
    // the same company name at the same domain -- an ordinal, which is stable
    // as long as the file's row order is.
    for (int n = 2; n < 1000; ++n) {
        const std::string cand = base + "-" + std::to_string(n);
        if (free_of(cand)) return cand;
    }
    return base + "-x";
}

// ── Import ───────────────────────────────────────────────────────────────────

Roster registry_parse(const std::string& csv_text, ImportReport& rep) {
    Roster out;

    const CsvTable table = csv_parse(csv_text);
    if (table.empty()) { rep.failure = "the file is empty"; return out; }

    const int hrow = find_header(table);
    if (hrow < 0) {
        rep.failure =
            "no registry header row found -- this does not look like the CPPA "
            "data broker registry export";
        return out;
    }
    const Columns col = map_columns(table[hrow]);
    if (col.rights < 0)
        rep.notes.push_back("no rights-URL column: entries will carry no opt-out URL");
    if (col.fcra < 0)
        rep.notes.push_back("no FCRA column: no entry will be marked FCRA-regulated");
    if (col.geo < 0)
        rep.notes.push_back("no geolocation column: no entry will be marked as "
                            "collecting precise location");

    std::vector<std::string> taken;
    std::set<std::string>    claimed;   // host -> already assigned to an entry

    for (std::size_t r = static_cast<std::size_t>(hrow) + 1; r < table.size(); ++r) {
        const CsvRow& row = table[r];
        const bool blank = std::all_of(row.begin(), row.end(),
                                       [](const std::string& s) { return trim(s).empty(); });
        if (blank) continue;

        rep.rows_read++;

        Broker b;
        b.name = cell(row, col.name);
        if (b.name.empty()) {
            rep.rows_skipped++;
            rep.notes.push_back("row " + std::to_string(r + 1) + ": no registrant name");
            continue;
        }

        const std::vector<std::string> urls = split_urls(cell(row, col.site));
        if (urls.empty()) {
            rep.rows_skipped++;
            rep.notes.push_back("row " + std::to_string(r + 1) + " (" + b.name +
                                "): no website filed");
            continue;
        }
        b.site = urls.front();

        // ── The host is the matching key; the registrant is the filing unit ──
        // They are not one-to-one in either direction. Seven registrants file
        // ten domains each. Eight separate companies file the SAME domain --
        // TransUnion's subsidiaries all point at transunion.com, and each of
        // them is a distinct legal entity with its own contact address that
        // receives its own request.
        //
        // So a host is claimed by its first filer, because two entries
        // claiming one host would make broker_for_url pick by roster order --
        // by accident. But losing the claim must NOT lose the registrant: an
        // entry with no hosts of its own is unreachable by URL match and fully
        // reachable by name and by email, which is the channel that files.
        // Dropping those nineteen rows would have deleted Equifax Information
        // Services from a roster that still listed Equifax.
        bool any_host = false;
        for (const auto& u : urls) {
            const std::string h = import_host_of(u);
            if (h.empty()) continue;
            any_host = true;
            if (std::find(b.hosts.begin(), b.hosts.end(), h) != b.hosts.end()) continue;
            if (!claimed.insert(h).second) {
                rep.shared_host_rows++;
                rep.notes.push_back(b.name + ": shares " + h +
                                    " with an earlier registrant -- listed, but a URL "
                                    "on that host matches the earlier one");
                continue;
            }
            b.hosts.push_back(h);
        }
        if (!any_host) {
            rep.rows_skipped++;
            rep.notes.push_back(b.name + ": no usable hostname in the website field");
            continue;
        }
        if (b.hosts.size() > 1) rep.multi_host_rows++;
        rep.hosts_total += static_cast<int>(b.hosts.size());

        // One registrant files two addresses in the single email field. Take
        // the first and say so, rather than storing a string that is not an
        // address and failing at send time in s15.
        const std::vector<std::string> mails = split_urls(cell(row, col.email));
        for (const auto& m : mails) {
            if (m.find('@') == std::string::npos) continue;
            if (b.opt_out_email.empty()) b.opt_out_email = m;
            else rep.notes.push_back(b.name + ": filed more than one contact address; "
                                              "using the first");
        }

        b.opt_out_url    = cell(row, col.rights);
        b.method         = Method::Email;   // see the header
        b.ca_registered  = true;            // it is the CA registry
        b.fcra_regulated = yes(cell(row, col.fcra));
        b.collects_geo   = yes(cell(row, col.geo));
        b.recheck_days   = 45;              // the DROP statute's own rhythm

        if (b.opt_out_email.empty()) {
            // Not a skip: a registrant with a rights page and no address is
            // still a broker worth listing, it just cannot be emailed.
            b.method = Method::Web;
            rep.notes.push_back(b.name + ": no contact address filed; web only");
        }
        if (b.method == Method::Web && b.opt_out_url.empty()) {
            b.method = Method::Unknown;
            rep.notes.push_back(b.name + ": neither an address nor a rights page");
        }

        // No unclaimed host means the id has to come from the name -- mint_id
        // falls back to it when the host stem is empty.
        b.id = mint_id(b.hosts.empty() ? std::string{} : b.hosts.front(), b.name, taken);
        taken.push_back(b.id);
        out.push_back(std::move(b));
    }
    return out;
}

// ── Merge ────────────────────────────────────────────────────────────────────

Roster roster_merge(const Roster& existing, const Roster& incoming,
                    ImportReport& rep) {
    // host -> index into `existing`, in two passes and the ORDER MATTERS.
    // Declared hosts first, so an owner always wins the key. Only then the
    // site's own host, and only for entries that declared nothing -- which is
    // what a roster written before Broker::hosts existed looks like, and is
    // how a hand-added or legacy entry gets adopted by its registry row.
    std::map<std::string, std::size_t> by_host;
    for (std::size_t i = 0; i < existing.size(); ++i)
        for (const auto& h : existing[i].hosts) by_host.emplace(h, i);
    for (std::size_t i = 0; i < existing.size(); ++i) {
        if (!existing[i].hosts.empty()) continue;
        const std::string sh = import_host_of(existing[i].site);
        if (!sh.empty()) by_host.emplace(sh, i);
    }

    // ── The sibling filers ───────────────────────────────────────────────────
    // Nineteen registrants own no host, because a sibling company filed the
    // domain first: IXI Corporation and Equifax Information Services both sit
    // at equifax.com. Matching those by host is not merely weak, it is
    // GUARANTEED WRONG -- the host resolves to the sibling, so a second import
    // would overwrite Equifax Inc's record with IXI's and orphan the entry it
    // meant to update. That is a roster that corrupts a little more every year.
    //
    // Name plus contact address separates seventeen of the nineteen. The
    // remaining two are a genuine triplicate -- the same company name at the
    // same address filed three times -- so identical keys are consumed in
    // order, which is stable for a given file and is the only thing that can
    // be stable about rows that are indistinguishable.
    const auto sibling_key = [](const Broker& b) {
        return b.name + "\x1f" + b.opt_out_email;
    };
    std::map<std::string, std::vector<std::size_t>> by_sibling;
    for (std::size_t i = 0; i < existing.size(); ++i)
        if (existing[i].hosts.empty()) by_sibling[sibling_key(existing[i])].push_back(i);
    std::map<std::string, std::size_t> sibling_cursor;

    Roster out = existing;
    std::vector<bool> touched(existing.size(), false);
    std::vector<std::string> ids;
    for (const auto& b : existing) ids.push_back(b.id);

    for (const auto& in : incoming) {
        std::size_t hit = out.size();
        if (!in.hosts.empty()) {
            for (const auto& h : in.hosts) {
                const auto it = by_host.find(h);
                if (it != by_host.end()) { hit = it->second; break; }
            }
        } else {
            // Deliberately NOT by_host -- see the note above. This entry's
            // domain belongs to somebody else and looking it up would find
            // them.
            const std::string k = sibling_key(in);
            const auto it = by_sibling.find(k);
            if (it != by_sibling.end()) {
                std::size_t& n = sibling_cursor[k];
                if (n < it->second.size()) hit = it->second[n++];
            }
        }

        if (hit < existing.size()) {
            Broker& dst = out[hit];
            touched[hit] = true;

            if (dst.name != in.name) {
                rep.merged_renamed++;
                rep.notes.push_back("'" + dst.name + "' now files as '" + in.name +
                                    "' -- id " + dst.id + " kept");
            }
            // Registry facts are the registry's. `id`, `notes`, `requires_id`
            // and `recheck_days` are not: the state does not know them and a
            // refresh must not blank a human's work.
            dst.name           = in.name;
            dst.site           = in.site;
            dst.opt_out_url    = in.opt_out_url;
            dst.opt_out_email  = in.opt_out_email;
            dst.method         = in.method;
            dst.ca_registered  = in.ca_registered;
            dst.fcra_regulated = in.fcra_regulated;
            dst.collects_geo   = in.collects_geo;

            // Union, never replace. A domain missing from this year's filing
            // may still be serving listings, and a case may point at it.
            for (const auto& h : in.hosts) {
                if (std::find(dst.hosts.begin(), dst.hosts.end(), h) == dst.hosts.end()) {
                    dst.hosts.push_back(h);
                    by_host.emplace(h, hit);
                }
            }
            rep.merged_updated++;
        } else {
            Broker nb = in;
            // The incoming ids were minted against the registry alone and can
            // collide with a hand-written entry.
            nb.id = mint_id(nb.hosts.empty() ? std::string{} : nb.hosts.front(),
                            nb.name, ids);
            ids.push_back(nb.id);
            for (const auto& h : nb.hosts) by_host.emplace(h, out.size());
            out.push_back(std::move(nb));
            rep.merged_new++;
        }
    }

    for (std::size_t i = 0; i < existing.size(); ++i) {
        if (touched[i]) continue;
        rep.merged_kept++;
        rep.notes.push_back("kept '" + existing[i].name +
                            "' -- not in this registry, left untouched");
    }
    return out;
}

}  // namespace delr::core
