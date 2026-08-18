#include "importer.hpp"

#include "Paths.hpp"
#include "core/Broker.hpp"
#include "core/RosterImport.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace delr::importer {

int run(const std::string& csv_path, const std::string& out_path) {
    using namespace delr::core;

    const std::string out = out_path.empty() ? paths::roster_file() : out_path;

    std::ifstream in(csv_path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "delr: cannot read %s\n", csv_path.c_str());
        return 2;
    }
    std::ostringstream buf;
    buf << in.rdbuf();

    ImportReport rep;
    const Roster incoming = registry_parse(buf.str(), rep);
    if (!rep.failure.empty()) {
        std::fprintf(stderr, "delr: %s\n", rep.failure.c_str());
        return 2;
    }

    std::string load_err;
    const Roster existing = roster_load(out, &load_err);
    if (!load_err.empty())
        std::fprintf(stderr, "delr: existing roster unreadable (%s) -- treating as empty\n",
                     load_err.c_str());

    const Roster merged = roster_merge(existing, incoming, rep);

    std::printf("registry   %s\n", csv_path.c_str());
    std::printf("roster     %s\n", out.c_str());
    std::printf("\n");
    std::printf("  rows read            %d\n", rep.rows_read);
    if (rep.rows_skipped)
        std::printf("  rows skipped         %d\n", rep.rows_skipped);
    std::printf("  listing domains      %d  (%d registrants publish under more than one)\n",
                rep.hosts_total, rep.multi_host_rows);
    if (rep.shared_host_rows)
        std::printf("  shared a domain      %d  (listed; the first filer owns the match)\n",
                    rep.shared_host_rows);
    std::printf("\n");
    std::printf("  new                  %d\n", rep.merged_new);
    std::printf("  updated in place     %d\n", rep.merged_updated);
    if (rep.merged_renamed)
        std::printf("  renamed (id kept)    %d\n", rep.merged_renamed);
    std::printf("  kept, not in file    %d\n", rep.merged_kept);
    std::printf("  roster now holds     %zu\n", merged.size());

    // Everything the import could not do, in full. A count of successes with
    // the exceptions summarised away is how the seven multi-domain rows got
    // lost the first time this data was mapped.
    if (!rep.notes.empty()) {
        std::printf("\nnotes (%zu)\n", rep.notes.size());
        for (const auto& n : rep.notes) std::printf("  %s\n", n.c_str());
    }

    // The roster is an asset every consumer trusts, so a bad one does not get
    // written over a good one. Refusing here costs the user a re-run; writing
    // costs them a lookup table that silently never hits.
    const auto problems = roster_validate(merged);
    if (!problems.empty()) {
        std::fprintf(stderr, "\ndelr: NOT WRITTEN -- the merged roster fails validation:\n");
        for (const auto& p : problems) std::fprintf(stderr, "  %s\n", p.c_str());
        return 1;
    }

    if (!roster_save(out, merged)) {
        std::fprintf(stderr, "\ndelr: could not write %s\n", out.c_str());
        return 2;
    }
    std::printf("\nwritten.\n");
    return 0;
}

}  // namespace delr::importer
