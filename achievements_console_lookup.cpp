// achievements_console_lookup.cpp — Console handler lookup table

#include "achievements_console.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

// Console handlers from separate files
extern const console_handler_t g_console_nes;
extern const console_handler_t g_console_snes;
extern const console_handler_t g_console_genesis;
extern const console_handler_t g_console_psx;
extern const console_handler_t g_console_n64;
extern const console_handler_t g_console_gameboy;
extern const console_handler_t g_console_sms;
extern const console_handler_t g_console_neogeo;
extern const console_handler_t g_console_gba;
extern const console_handler_t g_console_megacd;
extern const console_handler_t g_console_atari2600;
extern const console_handler_t g_console_tgfx16;
extern const console_handler_t g_console_s32x;
extern const console_handler_t g_console_saturn;
extern const console_handler_t g_console_virtualboy;

// Master lookup table
static const console_handler_t *g_console_handlers[] = {
	&g_console_nes,
	&g_console_snes,
	&g_console_genesis,
	&g_console_psx,
	&g_console_n64,
	&g_console_gameboy,
	&g_console_sms,
	&g_console_neogeo,
	&g_console_gba,
	&g_console_megacd,
	&g_console_atari2600,
	&g_console_tgfx16,
	&g_console_s32x,
	&g_console_saturn,
	&g_console_virtualboy,
	NULL
};

const console_handler_t *get_console_handler_by_name(const char *core_name)
{
	if (!core_name) return NULL;

	// Check exact matches first
	for (int i = 0; g_console_handlers[i] != NULL; i++) {
		if (!strcasecmp(core_name, g_console_handlers[i]->name)) {
			return g_console_handlers[i];
		}
	}

	// Check aliases
	if (!strcasecmp(core_name, "MegaDrive")) {
		return &g_console_genesis;
	}
	if (!strcasecmp(core_name, "GBC")) {
		return &g_console_gameboy;  // GameBoy handler handles both GB and GBC
	}
	if (!strcasecmp(core_name, "MegaDrive32X")) {
		return &g_console_s32x;
	}

	return NULL;
}

const console_handler_t *get_console_handler_by_id(int console_id)
{
	for (int i = 0; g_console_handlers[i] != NULL; i++) {
		if (g_console_handlers[i]->console_id == console_id) {
			return g_console_handlers[i];
		}
	}

	// Special cases: GameBoy Color (ID 6) uses GameBoy handler (ID 4)
	if (console_id == 6) {
		return &g_console_gameboy;
	}
	// Game Gear (ID 15) uses SMS handler (ID 11)
	if (console_id == 15) {
		return &g_console_sms;
	}
	// PC Engine CD (ID 76) uses TG16 handler (ID 8)
	if (console_id == 76) {
		return &g_console_tgfx16;
	}
	// Famicom Disk System (ID 81) uses NES handler
	if (console_id == 81) {
		return &g_console_nes;
	}

	return NULL;
}

// Initialize all console handlers
void init_all_console_handlers(void)
{
	for (int i = 0; g_console_handlers[i] != NULL; i++) {
		if (g_console_handlers[i]->init) {
			g_console_handlers[i]->init();
		}
	}
}


// ---------------------------------------------------------------------------
// Shared SelAddr stall recovery
// ---------------------------------------------------------------------------

#include "achievements.h"
#include "ra_ramread.h"

int seladdr_check_stall_recovery(console_state_t *state, uint32_t resp_frame,
                                  const char *console_name)
{
        if (!achievements_stall_recovery_enabled()) return 0;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double stall_secs = (now.tv_sec - state->stall_time.tv_sec)
                + (now.tv_nsec - state->stall_time.tv_nsec) / 1e9;

        if (stall_secs >= 5.0 && state->stall_frame == resp_frame) {
                ra_log_write("%s SelAddr: STALL RECOVERY -- resp_frame=%u stuck for %.1fs, re-collecting\n",
                        console_name, resp_frame, stall_secs);
                state->cache_ready = 0;
                state->needs_recollect = 0;
                ra_snes_addrlist_init();
                clock_gettime(CLOCK_MONOTONIC, &state->stall_time);
                state->stall_frame = 0;
                return 1;
        }
        return 0;
}

int seladdr_resync_if_backward(console_state_t *state, uint32_t resp_frame,
                                const char *console_name)
{
        if (resp_frame < state->last_resp_frame) {
                ra_log_write("%s SelAddr: resp_frame went backward (%u -> %u) -- FPGA reset without notify, re-bootstrapping\n",
                        console_name, state->last_resp_frame, resp_frame);
                state->last_resp_frame = resp_frame;
                clock_gettime(CLOCK_MONOTONIC, &state->stall_time);
                state->stall_frame = resp_frame;

                // The machine restarted behind our back. Realigning the frame
                // counter alone used to be the whole recovery, which left the
                // rc_client runtime armed and evaluating whatever RAM the
                // restarted core produced: an SMS "Eject ROM" (status[9], never
                // reported to the ARM) unlocked both of Dragon Wang's score
                // achievements from the garbage a cartridge-less Z80 writes.
                // Drop the cache instead, exactly as stall recovery does, so
                // the next poll re-runs the bootstrap -- that path re-primes
                // rc_client to WAITING before any value is evaluated.
                //
                // Every caller has the shape
                //     seladdr_resync_if_backward(...);
                //     if (resp_frame > state->last_resp_frame) { ...do_frame... }
                // so clearing last_resp_frame above already keeps this frame
                // from being evaluated; no caller needs to change.
                state->cache_ready     = 0;
                state->needs_recollect = 0;
                ra_snes_addrlist_init();
                return 1;
        }
        return 0;
}

// Shared gate for evaluating a frame in the smart-cache path.
//
// A response whose id does not match the current request_id means the FPGA's
// scan raced a list publish, so its VALCACHE can hold values from two
// different orderings. Indexing that with the active snapshot returns
// neighbouring addresses' bytes for the whole frame — delta real, mem garbage
// — which is what produced James Pond 3's burst of false unlocks. The frame's
// correct data no longer exists, so the choice is evaluating garbage or
// skipping one delta tick: we skip.
//
// The legacy paths always had this gate; only the smart-cache paths lacked it.
// With the publish-side guards in ra_snes_addrlist_flush_dynamic/prune_dynamic
// (busy check + freshness window) this should never fire — a hit in the log
// means a publish slipped past them and is worth investigating. Note that
// ra_snes_addrlist_end_collect (periodic recollect on GB/NeoGeo/PSX) has no
// such guard yet, so those cores are the likeliest to surface one.
// ---------------------------------------------------------------------------
// Trigger dump: rolling window of the value cache
//
// A false unlock is a *transition* ("delta below the threshold, current value
// above it"), so a snapshot taken when the achievement fires cannot explain
// it — the value that mattered is the one from the frame before. Keep the last
// few evaluated frames of the value cache and print the whole window when an
// achievement triggers, so the offending transition is visible.
//
// Opt-in (retroachievements.cfg: trigger_dump, and only with debug=1): the
// per-frame snapshot is a bulk read from uncached DDRAM — cheap for a small
// list, not free for a core tracking hundreds of addresses.
// ---------------------------------------------------------------------------
#define TRIGDUMP_FRAMES 8
// Frames to keep recording after a trigger before printing again. The frames up
// to the trigger cannot tell a false read from a real change; a false read goes
// back on the next frame (A,B,A) and a real one stays.
#define TRIGDUMP_POST_FRAMES 3

static uint8_t  s_td_vals[TRIGDUMP_FRAMES][RA_SNES_MAX_ADDRS];
static uint32_t s_td_addrs[RA_SNES_MAX_ADDRS];
static uint32_t s_td_frame[TRIGDUMP_FRAMES];
static int      s_td_count  = 0;
static int      s_td_head   = 0;
static int      s_td_filled = 0;
static int      s_td_post_left = 0;
static uint32_t s_td_post_id   = 0;
static char     s_td_post_title[96];

static void seladdr_trigdump_print(const char *tag, uint32_t ach_id, const char *title, int changed_only)
{
        int first = (s_td_head - s_td_filled + TRIGDUMP_FRAMES) % TRIGDUMP_FRAMES;

        char hdr[160];
        int p = 0;
        for (int f = 0; f < s_td_filled && p < (int)sizeof(hdr) - 12; f++)
                p += snprintf(hdr + p, sizeof(hdr) - p, " %u", s_td_frame[(first + f) % TRIGDUMP_FRAMES]);

        ra_log_write("%s [%u] %s: %d addrs, last %d evaluated frames (oldest->newest), resp_frame:%s\n",
                tag, ach_id, title ? title : "", s_td_count, s_td_filled, hdr);

        for (int i = 0; i < s_td_count; i++) {
                char line[128];
                int q = 0, changed = 0;
                uint8_t prev = 0;
                for (int f = 0; f < s_td_filled && q < (int)sizeof(line) - 4; f++) {
                        uint8_t v = s_td_vals[(first + f) % TRIGDUMP_FRAMES][i];
                        if (f && v != prev) changed = 1;
                        prev = v;
                        q += snprintf(line + q, sizeof(line) - q, " %02X", v);
                }
                if (changed_only && !changed) continue;
                // '*' marks an address that moved inside the window — the
                // transition that fired the achievement is on one of these.
                ra_log_write("  TD %c addr=0x%05X :%s\n",
                        changed ? '*' : ' ', s_td_addrs[i], line);
        }
}

static void seladdr_trigdump_snapshot(void *map)
{
        if (!achievements_trigger_dump() || !map) return;

        int n = ra_snes_addrlist_active_count();
        if (n <= 0) return;
        if (n > RA_SNES_MAX_ADDRS) n = RA_SNES_MAX_ADDRS;

        // Any list revision shifts every column, so a table spanning one would
        // not line up. Restart the window instead of printing a misleading one.
        const uint32_t *cur = ra_snes_addrlist_active_addrs();
        if (n != s_td_count || memcmp(s_td_addrs, cur, (size_t)n * sizeof(uint32_t))) {
                memcpy(s_td_addrs, cur, (size_t)n * sizeof(uint32_t));
                s_td_count  = n;
                s_td_filled = 0;
                s_td_head   = 0;
        }

        // One bulk read, not n binary searches + n uncached byte reads.
        memcpy(s_td_vals[s_td_head],
               (const uint8_t *)map + RA_SNES_VALCACHE_OFFSET + 8, (size_t)n);
        s_td_frame[s_td_head] = ra_snes_addrlist_response_frame(map);

        s_td_head = (s_td_head + 1) % TRIGDUMP_FRAMES;
        if (s_td_filled < TRIGDUMP_FRAMES) s_td_filled++;

        if (s_td_post_left > 0 && --s_td_post_left == 0)
                seladdr_trigdump_print("TRIGDUMP+POST", s_td_post_id, s_td_post_title, 1);
}

void seladdr_trigdump_report(uint32_t ach_id, const char *title)
{
        if (!achievements_trigger_dump() || s_td_filled == 0) return;

        seladdr_trigdump_print("TRIGDUMP", ach_id, title, 0);

        s_td_post_id = ach_id;
        snprintf(s_td_post_title, sizeof(s_td_post_title), "%s", title ? title : "");
        s_td_post_left = TRIGDUMP_POST_FRAMES;
}

int seladdr_frame_evaluable(void *map, const char *console_name)
{
        if (ra_snes_addrlist_is_ready(map)) {
                seladdr_trigdump_snapshot(map);
                return 1;
        }

        // Rate-limited: loud for the first few, then a periodic heartbeat, so a
        // persistent problem is visible without flooding the log at 60/s.
        static uint32_t skipped = 0;
        skipped++;
        if (skipped <= 10 || (skipped % 300) == 0) {
                ra_log_write("%s SelAddr: SKIP eval frame (resp id not aligned to req_id=%u, addrs=%d, total skipped=%u)\n",
                        console_name, ra_snes_addrlist_request_id(),
                        ra_snes_addrlist_count(), skipped);
        }
        return 0;
}
