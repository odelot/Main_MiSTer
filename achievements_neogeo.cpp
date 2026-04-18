// achievements_neogeo.cpp — RetroAchievements NeoGeo-specific implementation

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
// NeoGeo State
// ---------------------------------------------------------------------------

static console_state_t g_neogeo_state = {};

// ---------------------------------------------------------------------------
// NeoGeo Implementation
// ---------------------------------------------------------------------------

static void neogeo_init(void)
{
	memset(&g_neogeo_state, 0, sizeof(g_neogeo_state));
	ra_snes_addrlist_init();
}

static void neogeo_reset(void)
{
	g_neogeo_state.optionc = 0;
	g_neogeo_state.collecting = 0;
	g_neogeo_state.cache_ready = 0;
	g_neogeo_state.last_resp_frame = 0;
	g_neogeo_state.game_frames = 0;
	g_neogeo_state.poll_logged = 0;
}

static uint32_t neogeo_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	if (g_neogeo_state.optionc) {
		if (g_neogeo_state.collecting) {
			for (uint32_t i = 0; i < num_bytes; i++)
				ra_snes_addrlist_add(address + i);
		}
		if (g_neogeo_state.cache_ready) {
			for (uint32_t i = 0; i < num_bytes; i++)
				buffer[i] = ra_snes_addrlist_read_cached(map, address + i);
			return num_bytes;
		}
		memset(buffer, 0, num_bytes);
		return num_bytes;
	}
	return 0;
}

static void neogeo_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
	if (!client || !game_loaded || !map || !g_neogeo_state.optionc) return;
	
	rc_client_t *rc_client = (rc_client_t *)client;

	if (ra_snes_addrlist_count() == 0 && !g_neogeo_state.cache_ready) {
		// Bootstrap
		g_neogeo_state.collecting = 1;
		ra_snes_addrlist_begin_collect();
		rc_client_do_frame(rc_client);
		g_neogeo_state.collecting = 0;
		int changed = ra_snes_addrlist_end_collect(map);
		if (changed) {
			ra_log_write("NeoGeo OptionC: Bootstrap collection done, %d addrs\n",
				ra_snes_addrlist_count());
		}
	} else if (!g_neogeo_state.cache_ready) {
		// Wait for cache
		if (ra_snes_addrlist_is_ready(map)) {
			g_neogeo_state.cache_ready = 1;
			g_neogeo_state.last_resp_frame = 0;
			g_neogeo_state.game_frames = 0;
			g_neogeo_state.poll_logged = 0;
			clock_gettime(CLOCK_MONOTONIC, &g_neogeo_state.cache_time);
			ra_log_write("NeoGeo OptionC: Cache active!\n");
		}
	} else {
		// Normal processing
		uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
		if (resp_frame > g_neogeo_state.last_resp_frame) {
			g_neogeo_state.last_resp_frame = resp_frame;
			g_neogeo_state.game_frames++;

			// Re-collect every ~5 min
			int re_collect = (g_neogeo_state.game_frames % 18000 == 0) && (g_neogeo_state.game_frames > 0);
			if (re_collect) {
				g_neogeo_state.collecting = 1;
				ra_snes_addrlist_begin_collect();
			}

			rc_client_do_frame(rc_client);

			if (re_collect) {
				g_neogeo_state.collecting = 0;
				if (ra_snes_addrlist_end_collect(map)) {
					ra_log_write("NeoGeo OptionC: Address list refreshed, %d addrs\n",
						ra_snes_addrlist_count());
				}
			}
		}
	}

	// Periodic log
	uint32_t milestone = g_neogeo_state.game_frames / 300;
	if (milestone > 0 && milestone != g_neogeo_state.poll_logged) {
		g_neogeo_state.poll_logged = milestone;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = (now.tv_sec - g_neogeo_state.cache_time.tv_sec)
			+ (now.tv_nsec - g_neogeo_state.cache_time.tv_nsec) / 1e9;
		ra_log_write("POLL(NeoGeo): resp_frame=%u game_frames=%u elapsed=%.1fs addrs=%d\n",
			g_neogeo_state.last_resp_frame, g_neogeo_state.game_frames, elapsed,
			ra_snes_addrlist_count());
	}
#endif
}

static int neogeo_calculate_hash(const char *rom_path, char *md5_hex_out)
{
#ifdef HAS_RCHEEVOS
	// NeoGeo requires rc_hash_generate_from_file for filename-based hashing
	char abs_path[1024];
	if (rom_path[0] != '/') {
		snprintf(abs_path, sizeof(abs_path), "/media/fat/%s", rom_path);
	} else {
		strncpy(abs_path, rom_path, sizeof(abs_path) - 1);
		abs_path[sizeof(abs_path) - 1] = '\0';
	}

	if (rc_hash_generate_from_file(md5_hex_out, 27, abs_path)) {
		ra_log_write("NeoGeo hash: %s\n", md5_hex_out);
		return 1;
	}
	ra_log_write("NeoGeo hash failed\n");
#endif
	return 0;
}

static void neogeo_set_hardcore(int enabled)
{
	ra_log_write("NeoGeo: Hardcore mode %s (no FPGA bits mapped yet)\n",
		enabled ? "enabled" : "disabled");
}

// Called from main achievements code to detect protocol
void neogeo_detect_protocol(void *map)
{
	if (!map) return;
	const ra_header_t *hdr = (const ra_header_t *)map;
	if (hdr->region_count == 0) {
		g_neogeo_state.optionc = 1;
		ra_log_write("NeoGeo FPGA protocol: Option C\n");
	}
}

// ---------------------------------------------------------------------------
// Console handler definition
// ---------------------------------------------------------------------------

const console_handler_t g_console_neogeo = {
	.init = neogeo_init,
	.reset = neogeo_reset,
	.read_memory = neogeo_read_memory,
	.poll = neogeo_poll,
	.calculate_hash = neogeo_calculate_hash,
	.set_hardcore = neogeo_set_hardcore,
	.console_id = 27,  // RC_CONSOLE_ARCADE
	.name = "NEOGEO"
};
