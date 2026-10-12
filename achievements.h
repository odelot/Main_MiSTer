#ifndef ACHIEVEMENTS_H
#define ACHIEVEMENTS_H

#include <stdint.h>

// RetroAchievements integration for MiSTer FPGA
//
// Lifecycle:
//   1. achievements_init() — called once at startup
//   2. achievements_load_game(rom_path) — called when a ROM is loaded
//   3. achievements_poll() — called every frame from the scheduler loop
//   4. achievements_unload_game() — called when core changes or ROM unloads
//   5. achievements_deinit() — called at shutdown
//
// Debug log is written to /tmp/ra_debug.log and stdout (magenta prefix "RA:")

// Write a line to the RA log (stdout + /tmp/ra_debug.log). Safe to call from any thread.
void ra_log_write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Initialize the RA subsystem. Call once after core type is known.
void achievements_init(void);

// Notify that a new game ROM was loaded. Triggers MD5 hash + game identification.
// rom_path: full path to the ROM file, crc32: CRC32 from user_io_get_file_crc()
void achievements_load_game(const char *rom_path, uint32_t crc32);

// Per-frame poll. Checks DDRAM mirror for new frame data, calls rc_client_do_frame().
void achievements_poll(void);

// Unload current game (e.g., before loading a new one or switching cores).
void achievements_unload_game(void);

// Notify RA runtime that an in-core reset happened (without unloading the game).
void achievements_notify_core_reset(void);

// Notify RA runtime that a savestate was restored (softcore only — hardcore
// blocks state loads in RTL). Emulators serialize the rcheevos runtime into the
// savestate and restore it here, which carries the delta baseline (each
// memref's 'prior') across the load. MiSTer savestates are produced by the core
// in the FPGA, so there is no room for that blob and we are permanently in the
// "no achievement data in this state" case, which rc_client handles by
// resetting the runtime. NOT the same as a core reset: this must not satisfy
// the hardcore waiting_for_reset gate. See achievements_notify_state_loaded().
void achievements_notify_state_loaded(void);

// Notify that save-RAM I/O between the core and the ARM is happening (autosave
// on OSD-open, manual save/load backup). Cores that share the save port with
// the RA read path (e.g. SNES BSRAM Port B) return bytes from the SD transfer
// pointer instead of the requested address while the transfer runs, so
// achievement evaluation is suspended for the burst plus a grace period.
// Call once per serviced sector (cheap: just stamps a timestamp).
void achievements_notify_save_io(void);

// Shutdown. Frees all resources.
void achievements_deinit(void);

// Returns 1 if RA is active (game loaded and mirror functional)
int achievements_active(void);

// Show RA status info popup (login, game, achievement progress).
// Safe to call anytime — silently does nothing if menu is active.
void achievements_info(void);

// Returns 1 if hardcore mode is enabled in retroachievements.cfg
int achievements_hardcore_active(void);

// Returns 1 if the user is logged in and a game is currently loaded.
int achievements_has_active_game(void);

// Open the achievement list view (builds sorted list: unlocked first, then locked).
// Returns the total number of achievements, or 0 if not available.
int achievements_list_open(void);

// Close and free the achievement list view.
void achievements_list_close(void);

// Navigate the list. Use SCANF_NEXT, SCANF_PREV, SCANF_NEXT_PAGE, etc. from file_io.h.
void achievements_list_scan(int mode);

// Render the current page of the achievement list to the OSD.
void achievements_list_print(void);

// Animate the description ticker across the list view's bottom row. Call once
// per UI tick while the list is shown (no-op unless list_desc_ticker is set).
void achievements_list_ticker(void);

// 1 if the row highlighted in the list view is a real achievement (not a
// section header or progress sub-line). Gates the detail screen.
int achievements_list_selected_is_ach(void);

// Render the full-screen detail view (title, points/state, progress and the
// word-wrapped description) of the achievement highlighted in the list.
void achievements_detail_print(void);

// 1 = description ticker enabled in the list view (retroachievements.cfg: list_desc_ticker)
int achievements_desc_ticker_enabled(void);

// 1 = in-game Menu+Y gamepad shortcut to the list enabled (retroachievements.cfg: list_hotkey)
int achievements_list_hotkey_enabled(void);

// Returns the total count in the currently open list (0 if not open).
int achievements_list_count(void);

// Switch the list view to the previous/next achievement set (dir = -1/+1)
// of a multiset game (rcheevos 12+). Returns 1 if the view changed,
// 0 for single-set games (caller should fall back to page scrolling).
int achievements_list_switch_set(int dir);

// Update global frame counters (called by per-console poll handlers)
void ra_frame_processed(uint32_t frame);
int achievements_stall_recovery_enabled(void);
int achievements_rtquery_enabled(void);
int achievements_gba_reset_ram(void);   // 1 = clear IWRAM+EWRAM on game load (retroachievements.cfg: gba_reset_ram)
int achievements_recollect_interval(void);
int achievements_smart_cache_enabled(void);
int achievements_n64_snapshot_enabled(void);
int achievements_psx_blanking_sync(void); // 1 = PSX RA DDR3 traffic only during blanking (retroachievements.cfg: psx_blanking_sync)

// Debug watch list from retroachievements.cfg (watch=<hex>,<hex>,...).
// Returns the count and sets *addrs to the RA-address array. Console handlers
// log every per-frame value change of these addresses.
int achievements_watch_list(const uint32_t **addrs);

// 1 = dynamic-only smart-cache prune enabled (retroachievements.cfg:
// smart_cleanup, default 1). Drops add_dynamic (AddAddress) entries ~1/min
// when they pile up; the static bootstrap set is never pruned, and pruned
// addresses that are still needed re-add themselves via rtquery misses.
int achievements_smart_cleanup_enabled(void);
// 1 when the per-frame value-cache window for trigger dumps should be kept.
// Implies debug: with logging off the dump could never be printed, so the
// per-frame snapshot would be pure cost.
int achievements_trigger_dump(void);

// 1 = MegaDrive-only lightgun A/B test (retroachievements.cfg: justifier_test,
// default 0): keeps the smart cache but caps the rtquery busy-wait (~1ms,
// plus a 20ms fail-fast cooldown after a timeout) so no spin can delay
// input forwarding.
int achievements_justifier_test(void);

// Main-loop stall probe (retroachievements.cfg debug=1; no-op otherwise).
// The scheduler and the disc read path tag sections of the main loop so a
// stall between two achievements_poll calls can be attributed in the RA log.
enum {
	RA_PROBE_UIO,    // user_io_poll (includes RA_PROBE_CD)
	RA_PROBE_CD,     // PSX disc image read (buffer refill)
	RA_PROBE_FRAME,  // frame_timer
	RA_PROBE_INPUT,  // input_poll
	RA_PROBE_VIDEO,  // video_poll
	RA_PROBE_UI,     // co_ui: HandleUI + OsdUpdate
	RA_PROBE_COUNT
};
uint64_t achievements_probe_begin(void);  // 0 when the probe is off
void     achievements_probe_end(int section, uint64_t begin);

struct RaProbeScope
{
	int section;
	uint64_t begin;
	RaProbeScope(int s) : section(s), begin(achievements_probe_begin()) {}
	~RaProbeScope() { if (begin) achievements_probe_end(section, begin); }
};

#endif // ACHIEVEMENTS_H
