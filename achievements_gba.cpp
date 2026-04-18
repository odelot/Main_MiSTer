// achievements_gba.cpp — RetroAchievements Game Boy Advance-specific implementation

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
#endif

// ---------------------------------------------------------------------------
// GBA State
// ---------------------------------------------------------------------------

static console_state_t g_gba_state = {};

// ---------------------------------------------------------------------------
// GBA Implementation
// ---------------------------------------------------------------------------

static void gba_init(void)
{
	memset(&g_gba_state, 0, sizeof(g_gba_state));
	ra_snes_addrlist_init();
}

static void gba_reset(void)
{
	g_gba_state.optionc = 0;
	g_gba_state.collecting = 0;
	g_gba_state.cache_ready = 0;
	g_gba_state.last_resp_frame = 0;
	g_gba_state.game_frames = 0;
	g_gba_state.poll_logged = 0;
}

static uint32_t gba_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	if (g_gba_state.optionc) {
		if (g_gba_state.collecting) {
			for (uint32_t i = 0; i < num_bytes; i++)
				ra_snes_addrlist_add(address + i);
		}
		if (g_gba_state.cache_ready) {
			for (uint32_t i = 0; i < num_bytes; i++)
				buffer[i] = ra_snes_addrlist_read_cached(map, address + i);
			return num_bytes;
		}
		memset(buffer, 0, num_bytes);
		return num_bytes;
	}
	return 0;
}

static void gba_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
	if (!client || !game_loaded || !map || !g_gba_state.optionc) return;
	
	rc_client_t *rc_client = (rc_client_t *)client;

	if (ra_snes_addrlist_count() == 0 && !g_gba_state.cache_ready) {
		// Bootstrap
		g_gba_state.collecting = 1;
		ra_snes_addrlist_begin_collect();
		rc_client_do_frame(rc_client);
		g_gba_state.collecting = 0;
		int changed = ra_snes_addrlist_end_collect(map);
		if (changed) {
			ra_log_write("GBA OptionC: Bootstrap collection done, %d addrs\n",
				ra_snes_addrlist_count());
		}
	} else if (!g_gba_state.cache_ready) {
		// Wait for cache
		if (ra_snes_addrlist_is_ready(map)) {
			g_gba_state.cache_ready = 1;
			g_gba_state.last_resp_frame = 0;
			g_gba_state.game_frames = 0;
			g_gba_state.poll_logged = 0;
			clock_gettime(CLOCK_MONOTONIC, &g_gba_state.cache_time);
			ra_log_write("GBA OptionC: Cache active!\n");
		}
	} else {
		// Normal processing
		uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
		if (resp_frame > g_gba_state.last_resp_frame) {
			g_gba_state.last_resp_frame = resp_frame;
			g_gba_state.game_frames++;

			// Re-collect every ~5 min
			int re_collect = (g_gba_state.game_frames % 18000 == 0) && (g_gba_state.game_frames > 0);
			if (re_collect) {
				g_gba_state.collecting = 1;
				ra_snes_addrlist_begin_collect();
			}

			rc_client_do_frame(rc_client);

			if (re_collect) {
				g_gba_state.collecting = 0;
				if (ra_snes_addrlist_end_collect(map)) {
					ra_log_write("GBA OptionC: Address list refreshed, %d addrs\n",
						ra_snes_addrlist_count());
				}
			}
		}
	}

	// Periodic log
	uint32_t milestone = g_gba_state.game_frames / 300;
	if (milestone > 0 && milestone != g_gba_state.poll_logged) {
		g_gba_state.poll_logged = milestone;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = (now.tv_sec - g_gba_state.cache_time.tv_sec)
			+ (now.tv_nsec - g_gba_state.cache_time.tv_nsec) / 1e9;
		ra_log_write("POLL(GBA): resp_frame=%u game_frames=%u elapsed=%.1fs addrs=%d\n",
			g_gba_state.last_resp_frame, g_gba_state.game_frames, elapsed,
			ra_snes_addrlist_count());
	}
#endif
}

static int gba_calculate_hash(const char *rom_path, char *md5_hex_out)
{
	(void)rom_path;
	(void)md5_hex_out;
	// GBA uses standard MD5
	return 0; // Let main code handle it
}

static void gba_set_hardcore(int enabled)
{
	ra_log_write("GBA: Hardcore mode %s (no FPGA bits mapped yet)\n",
		enabled ? "enabled" : "disabled");
}

// Called from main achievements code to detect protocol
void gba_detect_protocol(void *map)
{
	if (!map) return;
	const ra_header_t *hdr = (const ra_header_t *)map;
	if (hdr->region_count == 0) {
		g_gba_state.optionc = 1;
		ra_log_write("GBA FPGA protocol: Option C\n");
	}
}

// ---------------------------------------------------------------------------
// Console handler definition
// ---------------------------------------------------------------------------

const console_handler_t g_console_gba = {
	.init = gba_init,
	.reset = gba_reset,
	.read_memory = gba_read_memory,
	.poll = gba_poll,
	.calculate_hash = gba_calculate_hash,
	.set_hardcore = gba_set_hardcore,
	.console_id = 5,  // RC_CONSOLE_GAMEBOY_ADVANCE
	.name = "GBA"
};
