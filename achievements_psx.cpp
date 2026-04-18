// achievements_psx.cpp — RetroAchievements PlayStation-specific implementation

#include "achievements_console.h"
#include "achievements.h"
#include "ra_ramread.h"
#include "user_io.h"
#include <string.h>
#include <time.h>
#include <stdio.h>

#ifdef HAS_RCHEEVOS
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#endif

// ---------------------------------------------------------------------------
// PSX State
// ---------------------------------------------------------------------------

static console_state_t g_psx_state = {};

// ---------------------------------------------------------------------------
// PSX Implementation
// ---------------------------------------------------------------------------

static void psx_init(void)
{
	memset(&g_psx_state, 0, sizeof(g_psx_state));
	ra_snes_addrlist_init();
}

static void psx_reset(void)
{
	g_psx_state.optionc = 0;
	g_psx_state.collecting = 0;
	g_psx_state.cache_ready = 0;
	g_psx_state.needs_recollect = 0;
	g_psx_state.last_resp_frame = 0;
	g_psx_state.game_frames = 0;
	g_psx_state.poll_logged = 0;
}

static uint32_t psx_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	if (g_psx_state.optionc) {
		if (g_psx_state.collecting) {
			for (uint32_t i = 0; i < num_bytes; i++)
				ra_snes_addrlist_add(address + i);
		}
		if (g_psx_state.cache_ready) {
			for (uint32_t i = 0; i < num_bytes; i++)
				buffer[i] = ra_snes_addrlist_read_cached(map, address + i);
			return num_bytes;
		}
		memset(buffer, 0, num_bytes);
		return num_bytes;
	}
	return 0;
}

static void psx_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
	if (!client || !game_loaded || !map || !g_psx_state.optionc) return;
	
	rc_client_t *rc_client = (rc_client_t *)client;

	// PSX has 5 phases: bootstrap, wait, pointer-resolution re-collect, wait again, normal
	if (ra_snes_addrlist_count() == 0 && !g_psx_state.cache_ready) {
		// Phase 1: Bootstrap collection
		g_psx_state.collecting = 1;
		ra_snes_addrlist_begin_collect();
		rc_client_do_frame(rc_client);
		g_psx_state.collecting = 0;
		int changed = ra_snes_addrlist_end_collect(map);
		if (changed) {
			ra_log_write("PSX OptionC: Bootstrap collection done, %d addrs\n",
				ra_snes_addrlist_count());
			g_psx_state.needs_recollect = 1;
		}
	} else if (!g_psx_state.cache_ready && g_psx_state.needs_recollect) {
		// Phase 2: Wait for first cache
		if (ra_snes_addrlist_is_ready(map)) {
			ra_log_write("PSX OptionC: Pre-recollect cache ready, doing pointer resolution pass\n");
			// Phase 3: Pointer-resolution re-collection
			g_psx_state.collecting = 1;
			ra_snes_addrlist_begin_collect();
			rc_client_do_frame(rc_client);
			g_psx_state.collecting = 0;
			if (ra_snes_addrlist_end_collect(map)) {
				ra_log_write("PSX OptionC: Pointer-resolution done, %d addrs\n",
					ra_snes_addrlist_count());
			}
			g_psx_state.needs_recollect = 0;
		}
	} else if (!g_psx_state.cache_ready) {
		// Phase 4: Wait for final cache
		if (ra_snes_addrlist_is_ready(map)) {
			g_psx_state.cache_ready = 1;
			g_psx_state.last_resp_frame = 0;
			g_psx_state.game_frames = 0;
			g_psx_state.poll_logged = 0;
			clock_gettime(CLOCK_MONOTONIC, &g_psx_state.cache_time);
			ra_log_write("PSX OptionC: Final cache active!\n");
		}
	} else {
		// Phase 5: Normal processing
		uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
		if (resp_frame > g_psx_state.last_resp_frame) {
			g_psx_state.last_resp_frame = resp_frame;
			g_psx_state.game_frames++;

			// Re-collect more frequently for PSX (every 600 frames ~10s)
			int re_collect = (g_psx_state.game_frames % 600 == 0) && (g_psx_state.game_frames > 0);
			if (re_collect) {
				g_psx_state.collecting = 1;
				ra_snes_addrlist_begin_collect();
			}

			rc_client_do_frame(rc_client);

			if (re_collect) {
				g_psx_state.collecting = 0;
				if (ra_snes_addrlist_end_collect(map)) {
					ra_log_write("PSX OptionC: Address list refreshed, %d addrs\n",
						ra_snes_addrlist_count());
				}
			}
		}
	}

	// Periodic log
	uint32_t milestone = g_psx_state.game_frames / 300;
	if (milestone > 0 && milestone != g_psx_state.poll_logged) {
		g_psx_state.poll_logged = milestone;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = (now.tv_sec - g_psx_state.cache_time.tv_sec)
			+ (now.tv_nsec - g_psx_state.cache_time.tv_nsec) / 1e9;
		ra_log_write("POLL(PSX): resp_frame=%u game_frames=%u elapsed=%.1fs addrs=%d\n",
			g_psx_state.last_resp_frame, g_psx_state.game_frames, elapsed,
			ra_snes_addrlist_count());
	}
#endif
}

static int psx_calculate_hash(const char *rom_path, char *md5_hex_out)
{
#ifdef HAS_RCHEEVOS
	// PSX requires rc_hash_generate_from_file for CD/ISO images
	char abs_path[1024];
	if (rom_path[0] != '/') {
		snprintf(abs_path, sizeof(abs_path), "/media/fat/%s", rom_path);
	} else {
		strncpy(abs_path, rom_path, sizeof(abs_path) - 1);
		abs_path[sizeof(abs_path) - 1] = '\0';
	}

	if (rc_hash_generate_from_file(md5_hex_out, 12, abs_path)) {
		ra_log_write("PSX hash: %s\n", md5_hex_out);
		return 1;
	}
	ra_log_write("PSX hash failed\n");
#endif
	return 0;
}

static void psx_set_hardcore(int enabled)
{
	if (enabled) {
		user_io_status_set("[93]", 1);  // Disable cheats
		user_io_status_set("[6]", 1);   // Disable save states
		ra_log_write("PSX: Hardcore mode enabled\n");
	} else {
		user_io_status_set("[93]", 0);
		user_io_status_set("[6]", 0);
		ra_log_write("PSX: Hardcore mode disabled\n");
	}
}

// Called from main achievements code to detect protocol
void psx_detect_protocol(void *map)
{
	if (!map) return;
	const ra_header_t *hdr = (const ra_header_t *)map;
	if (hdr->region_count == 0) {
		g_psx_state.optionc = 1;
		ra_log_write("PSX FPGA protocol: Option C\n");
	}
}

// ---------------------------------------------------------------------------
// Console handler definition
// ---------------------------------------------------------------------------

const console_handler_t g_console_psx = {
	.init = psx_init,
	.reset = psx_reset,
	.read_memory = psx_read_memory,
	.poll = psx_poll,
	.calculate_hash = psx_calculate_hash,
	.set_hardcore = psx_set_hardcore,
	.console_id = 12,  // RC_CONSOLE_PLAYSTATION
	.name = "PSX"
};
