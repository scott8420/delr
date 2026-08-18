#pragma once
#include <spdlog/spdlog.h>
#include <memory>

// Per-area logging, runtime-toggleable (CANON: per-area logging areas with
// runtime toggle). Areas are named by CONCERN, not by file.
//
// delr rule, non-negotiable: NO AREA EVER LOGS A PII FIELD VALUE. Log the
// broker id, the field NAME, the outcome -- never the name, address, phone,
// email or DOB being matched. A privacy tool that leaves a dossier in a
// terminal scrollback has defeated itself.
namespace delr::log {

enum class Area { App, Shell, Registry, Roster, Cases, Io };

std::shared_ptr<spdlog::logger> get(Area area);
void init();
void set_level(Area area, spdlog::level::level_enum level);

}  // namespace delr::log
