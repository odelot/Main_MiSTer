#pragma once

#ifdef HAS_RCHEEVOS
// Registers a unified CD reader that supports:
//   - .chd  via mister_load_chd / mister_chd_read_sector
//   - .cue / .gdi  via the rcheevos default handlers (unchanged behaviour)
//
// Call once during achievements_init(), replacing rc_hash_init_default_cdreader().
// All disc-based consoles (PSX, MegaCD, PCE-CD, NeoGeoCD) benefit automatically.
void ra_cdreader_chd_register(void);
#endif
