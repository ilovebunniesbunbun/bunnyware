#pragma once

// Automatic, in-process refresh of the CS2 offsets that BUNNYWARE actually
// consumes. Everything is resolved from the live cs2 process using
// process_vm_readv only (module discovery uses /proc/<pid>/maps), so the
// external cs2-linux dumper is no longer required for normal updates.
//
// The refresh runs on startup from GameReader::initialize() and can also be
// invoked manually with `bunnyware --update-offsets`. Only values that are
// present in the consolidated offsets.json are refreshed; anything else
// (full schema tables, unused offsets, BUNNYWARE-specific match-session values)
// is preserved untouched.

#include <string>
#include <vector>

namespace offset_refresher {

struct RefreshStatus {
    bool attempted = false;         // a refresh was started (not SHM test mode)
    bool target_found = false;      // a running cs2 process was located
    bool success = false;           // every required signature resolved; offsets written
    bool wrote_file = false;        // offsets.json (or a synced copy) was rewritten
    int base_offsets_resolved = 0;  // signature-derived offsets that resolved
    int base_offsets_changed = 0;   // of those, how many differ from offsets.json
    int schema_fields_resolved = 0; // needed schema fields found in the live schema
    int schema_fields_changed = 0;  // of those, how many differ from offsets.json
    int schema_fields_missing = 0;  // needed schema fields not present in this build
    bool cvar_interface_resolved = false; // tier0_VEngineCvar fallback refreshed
    std::vector<std::string> errors;   // signature failures / critical problems
    std::vector<std::string> warnings; // missing schema fields, unreadable modules, ...
    std::string summary;               // compact human-readable status

    bool healthy() const { return attempted && success; }
};

// Resolves and merges the live offsets into offsets.json (plus any synced copy
// such as build/offsets.json). Safe to call when cs2 is not running or when
// the process cannot be read: it reports the problem and leaves files alone.
RefreshStatus refresh_from_live_process();

// Status of the most recent refresh (for the debug menu / callers).
const RefreshStatus& last_status();

} // namespace offset_refresher
