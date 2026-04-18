// achievements_n64.cpp — RetroAchievements Nintendo 64-specific implementation

#include "achievements_console.h"
#include "achievements.h"
#include "ra_ramread.h"
#include "user_io.h"
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef HAS_RCHEEVOS
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#endif

// ---------------------------------------------------------------------------
// N64 State
// ---------------------------------------------------------------------------

static console_state_t g_n64_state = {};
static uint8_t *g_n64_progress_buf = NULL;
static size_t   g_n64_progress_size = 0;

// ---------------------------------------------------------------------------
// N64 Implementation
// ---------------------------------------------------------------------------

static void n64_init(void)
{
	memset(&g_n64_state, 0, sizeof(g_n64_state));
	g_n64_progress_buf = NULL;
	g_n64_progress_size = 0;
	ra_snes_addrlist_init();
	
	// N64 uses special DDRAM base to avoid save state collision
	ra_ramread_set_base(0x38000000);
	ra_log_write("N64: Using RA DDRAM base 0x38000000 (avoiding SS slot collision)\n");
}

static void n64_reset(void)
{
	g_n64_state.optionc = 0;
	g_n64_state.collecting = 0;
	g_n64_state.cache_ready = 0;
	g_n64_state.needs_recollect = 0;
	g_n64_state.last_resp_frame = 0;
	g_n64_state.game_frames = 0;
	g_n64_state.poll_logged = 0;
	g_n64_state.resolve_pass = 0;
	if (g_n64_progress_buf) { free(g_n64_progress_buf); g_n64_progress_buf = NULL; }
	g_n64_progress_size = 0;
}

static uint32_t n64_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	if (g_n64_state.optionc) {
		// N64 rcheevos addresses are big-endian; RDRAM in DDR3 is little-endian
		// within 32-bit words. XOR 3 on the low 2 bits converts between byte orders.
		if (g_n64_state.collecting) {
			for (uint32_t i = 0; i < num_bytes; i++)
				ra_snes_addrlist_add(((address + i) ^ 3));  // XOR 3 for byte-order
		}
		if (g_n64_state.cache_ready) {
			for (uint32_t i = 0; i < num_bytes; i++)
				buffer[i] = ra_snes_addrlist_read_cached(map, ((address + i) ^ 3));
			return num_bytes;
		}
		memset(buffer, 0, num_bytes);
		return num_bytes;
	}
	return 0;
}

static void n64_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
	if (!client || !game_loaded || !map || !g_n64_state.optionc) return;
	
	rc_client_t *rc_client = (rc_client_t *)client;

	// N64 has 5 phases like PSX: bootstrap, wait, pointer-resolution, wait, normal
	if (ra_snes_addrlist_count() == 0 && !g_n64_state.cache_ready) {
		// Phase 1: Bootstrap
		// Save rcheevos state before collection to prevent delta corruption
		size_t psize = rc_client_progress_size(rc_client);
		if (psize > g_n64_progress_size) {
			free(g_n64_progress_buf);
			g_n64_progress_buf = (uint8_t *)malloc(psize);
			g_n64_progress_size = psize;
		}
		if (g_n64_progress_buf)
			rc_client_serialize_progress_sized(rc_client, g_n64_progress_buf, g_n64_progress_size);
		g_n64_state.collecting = 1;
		ra_snes_addrlist_begin_collect();
		rc_client_do_frame(rc_client);
		g_n64_state.collecting = 0;
		// Restore state (undo delta/hit changes from zero-value reads)
		if (g_n64_progress_buf)
			rc_client_deserialize_progress_sized(rc_client, g_n64_progress_buf, g_n64_progress_size);
		int changed = ra_snes_addrlist_end_collect(map);
		if (changed) {
			ra_log_write("N64 OptionC: Bootstrap collection done, %d addrs\n",
				ra_snes_addrlist_count());
			g_n64_state.needs_recollect = 1;
			g_n64_state.resolve_pass = 0;
		}
	} else if (!g_n64_state.cache_ready && g_n64_state.needs_recollect) {
		// Phase 2 & 3: Iterative pointer-resolution for multi-level AddAddress
		if (ra_snes_addrlist_is_ready(map)) {
			g_n64_state.resolve_pass++;
			ra_log_write("N64 OptionC: Pointer resolution pass %d\n", g_n64_state.resolve_pass);
			g_n64_state.collecting = 1;
			ra_snes_addrlist_begin_collect();
			rc_client_do_frame(rc_client);
			g_n64_state.collecting = 0;
			int changed = ra_snes_addrlist_end_collect(map);
			if (changed && g_n64_state.resolve_pass < 4) {
				ra_log_write("N64 OptionC: Pass %d found new addrs, %d total - re-resolving\n",
					g_n64_state.resolve_pass, ra_snes_addrlist_count());
				// Stay in needs_recollect to do another pass once cache is ready
			} else {
				ra_log_write("N64 OptionC: Pointer-resolution %s after %d passes, %d addrs\n",
					changed ? "done (max passes)" : "stable",
					g_n64_state.resolve_pass, ra_snes_addrlist_count());
				g_n64_state.needs_recollect = 0;
			}
		}
	} else if (!g_n64_state.cache_ready) {
		// Phase 4: Final cache wait
		if (ra_snes_addrlist_is_ready(map)) {
			g_n64_state.cache_ready = 1;
			g_n64_state.last_resp_frame = 0;
			g_n64_state.game_frames = 0;
			g_n64_state.poll_logged = 0;
			clock_gettime(CLOCK_MONOTONIC, &g_n64_state.cache_time);
			ra_log_write("N64 OptionC: Cache active!\n");
			// Restore rcheevos state saved before resolution began
			if (g_n64_progress_buf)
				rc_client_deserialize_progress_sized(rc_client, g_n64_progress_buf, g_n64_progress_size);
		}
	} else {
		// Phase 5: Normal processing
		uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
		if (resp_frame > g_n64_state.last_resp_frame) {
			g_n64_state.last_resp_frame = resp_frame;
			g_n64_state.game_frames++;

			// Byte-order diagnostic at frames 30 and 300
			if (g_n64_state.game_frames == 30 || g_n64_state.game_frames == 300) {
				ra_log_write("N64: Byte-order diagnostic at frame %u\n", g_n64_state.game_frames);
				// Sample Super Mario 64 addresses
				uint8_t map_id = ra_snes_addrlist_read_cached(map, 0x0033b24a ^ 3);
				uint8_t hp = ra_snes_addrlist_read_cached(map, 0x0033b21d ^ 3);
				uint8_t lives = ra_snes_addrlist_read_cached(map, 0x0033b21e ^ 3);
				ra_log_write("  SM64: MapID=%02X HP=%02X Lives=%02X\n", map_id, hp, lives);
			}

			// Re-collect every 600 frames (~10s) with multi-pass for AddAddress
			int re_collect = (g_n64_state.game_frames % 600 == 0) && (g_n64_state.game_frames > 0);
			if (re_collect) {
				// Save state before re-collection
				size_t psize = rc_client_progress_size(rc_client);
				if (psize > g_n64_progress_size) {
					free(g_n64_progress_buf);
					g_n64_progress_buf = (uint8_t *)malloc(psize);
					g_n64_progress_size = psize;
				}
				if (g_n64_progress_buf)
					rc_client_serialize_progress_sized(rc_client, g_n64_progress_buf, g_n64_progress_size);
				g_n64_state.collecting = 1;
				ra_snes_addrlist_begin_collect();
			}

			rc_client_do_frame(rc_client);

			if (re_collect) {
				g_n64_state.collecting = 0;
				if (ra_snes_addrlist_end_collect(map)) {
					ra_log_write("N64 OptionC: Address list refreshed, %d addrs - scheduling re-resolve\n",
						ra_snes_addrlist_count());
					g_n64_state.cache_ready = 0;
					g_n64_state.needs_recollect = 1;
					g_n64_state.resolve_pass = 0;
				} else {
					// No change, restore state
					if (g_n64_progress_buf)
						rc_client_deserialize_progress_sized(rc_client, g_n64_progress_buf, g_n64_progress_size);
				}
			}
		}
	}

	// Periodic log every 100 frames (more frequent than other consoles)
	uint32_t milestone = g_n64_state.game_frames / 100;
	if (milestone > 0 && milestone != g_n64_state.poll_logged) {
		g_n64_state.poll_logged = milestone;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = (now.tv_sec - g_n64_state.cache_time.tv_sec)
			+ (now.tv_nsec - g_n64_state.cache_time.tv_nsec) / 1e9;
		ra_log_write("POLL(N64): resp_frame=%u game_frames=%u elapsed=%.1fs addrs=%d\n",
			g_n64_state.last_resp_frame, g_n64_state.game_frames, elapsed,
			ra_snes_addrlist_count());
	}
#endif
}

static int n64_calculate_hash(const char *rom_path, char *md5_hex_out)
{
#ifdef HAS_RCHEEVOS
	// N64 requires rc_hash_generate_from_file due to variable byte-order
	char abs_path[1024];
	if (rom_path[0] != '/') {
		snprintf(abs_path, sizeof(abs_path), "/media/fat/%s", rom_path);
	} else {
		strncpy(abs_path, rom_path, sizeof(abs_path) - 1);
		abs_path[sizeof(abs_path) - 1] = '\0';
	}

	if (rc_hash_generate_from_file(md5_hex_out, 2, abs_path)) {
		ra_log_write("N64 hash: %s\n", md5_hex_out);
		return 1;
	}
	ra_log_write("N64 hash failed\n");
#endif
	return 0;
}

static void n64_set_hardcore(int enabled)
{
	ra_log_write("N64: Hardcore mode %s (no FPGA bits mapped yet)\n", 
		enabled ? "enabled" : "disabled");
}

// Called from main achievements code to detect protocol
void n64_detect_protocol(void *map)
{
	if (!map) return;
	const ra_header_t *hdr = (const ra_header_t *)map;
	if (hdr->region_count == 0) {
		g_n64_state.optionc = 1;
		ra_log_write("N64 FPGA protocol: Option C\n");
	}
}

// ---------------------------------------------------------------------------
// Console handler definition
// ---------------------------------------------------------------------------

const console_handler_t g_console_n64 = {
	.init = n64_init,
	.reset = n64_reset,
	.read_memory = n64_read_memory,
	.poll = n64_poll,
	.calculate_hash = n64_calculate_hash,
	.set_hardcore = n64_set_hardcore,
	.console_id = 2,  // RC_CONSOLE_NINTENDO_64
	.name = "N64"
};
