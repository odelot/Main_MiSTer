// achievements_genesis.cpp — RetroAchievements Genesis/MegaDrive-specific implementation

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
// Genesis/MegaDrive State
// ---------------------------------------------------------------------------

static console_state_t g_md_state = {};

// ---------------------------------------------------------------------------
// Genesis/MegaDrive Implementation
// ---------------------------------------------------------------------------

static void genesis_init(void)
{
	memset(&g_md_state, 0, sizeof(g_md_state));
	ra_snes_addrlist_init();
}

static void genesis_reset(void)
{
	g_md_state.optionc = 0;
	g_md_state.collecting = 0;
	g_md_state.cache_ready = 0;
	g_md_state.last_resp_frame = 0;
	g_md_state.game_frames = 0;
	g_md_state.poll_logged = 0;
}

static uint32_t genesis_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	if (g_md_state.optionc) {
		if (g_md_state.collecting) {
			for (uint32_t i = 0; i < num_bytes; i++)
				ra_snes_addrlist_add(address + i);
		}
		if (g_md_state.cache_ready) {
			for (uint32_t i = 0; i < num_bytes; i++)
				buffer[i] = ra_snes_addrlist_read_cached(map, address + i);
			return num_bytes;
		}
		memset(buffer, 0, num_bytes);
		return num_bytes;
	}
	return 0;
}

static void genesis_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
	if (!client || !game_loaded || !map || !g_md_state.optionc) return;
	
	rc_client_t *rc_client = (rc_client_t *)client;

	if (ra_snes_addrlist_count() == 0 && !g_md_state.cache_ready) {
		// Bootstrap collection
		g_md_state.collecting = 1;
		ra_snes_addrlist_begin_collect();
		rc_client_do_frame(rc_client);
		g_md_state.collecting = 0;
		int changed = ra_snes_addrlist_end_collect(map);
		if (changed) {
			ra_log_write("Genesis OptionC: Bootstrap collection done, %d addrs\n",
				ra_snes_addrlist_count());
		}
	} else if (!g_md_state.cache_ready) {
		// Wait for cache
		if (ra_snes_addrlist_is_ready(map)) {
			g_md_state.cache_ready = 1;
			g_md_state.last_resp_frame = 0;
			g_md_state.game_frames = 0;
			g_md_state.poll_logged = 0;
			clock_gettime(CLOCK_MONOTONIC, &g_md_state.cache_time);
			ra_log_write("Genesis OptionC: Cache active!\n");
		}
	} else {
		// Normal frame processing
		uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
		if (resp_frame > g_md_state.last_resp_frame) {
			g_md_state.last_resp_frame = resp_frame;
			g_md_state.game_frames++;

			// Dump first 5 frames with Sonic 1 addresses
			if (g_md_state.game_frames <= 5) {
				const uint8_t *vals = (const uint8_t *)map + 0x48000 + 8;
				int cnt = ra_snes_addrlist_count();
				int dump_len = cnt < 32 ? cnt : 32;
				int non_zero = 0;
				char hex[256];
				int pos = 0;
				for (int i = 0; i < dump_len && pos < (int)sizeof(hex) - 4; i++) {
					pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", vals[i]);
					if (vals[i]) non_zero++;
				}
				ra_log_write("Genesis Frame %u: VALCACHE[0..%d]: %s (%d non-zero)\n", 
					g_md_state.game_frames, dump_len - 1, hex, non_zero);
				
				// Sonic 1 key addresses
				uint8_t gs   = ra_snes_addrlist_read_cached(map, 0xF601);
				uint8_t act  = ra_snes_addrlist_read_cached(map, 0xFE10);
				uint8_t zone = ra_snes_addrlist_read_cached(map, 0xFE11);
				uint8_t lives= ra_snes_addrlist_read_cached(map, 0xFE13);
				ra_log_write("  Sonic1: state=%02X act=%02X zone=%02X lives=%02X\n",
					gs, act, zone, lives);
			}

			// Re-collect every ~5 min
			int re_collect = (g_md_state.game_frames % 18000 == 0) && (g_md_state.game_frames > 0);
			if (re_collect) {
				g_md_state.collecting = 1;
				ra_snes_addrlist_begin_collect();
			}

			rc_client_do_frame(rc_client);

			if (re_collect) {
				g_md_state.collecting = 0;
				if (ra_snes_addrlist_end_collect(map)) {
					ra_log_write("Genesis OptionC: Address list refreshed, %d addrs\n",
						ra_snes_addrlist_count());
				}
			}
		}
	}

	// Periodic log
	uint32_t milestone = g_md_state.game_frames / 300;
	if (milestone > 0 && milestone != g_md_state.poll_logged) {
		g_md_state.poll_logged = milestone;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = (now.tv_sec - g_md_state.cache_time.tv_sec)
			+ (now.tv_nsec - g_md_state.cache_time.tv_nsec) / 1e9;
		ra_log_write("POLL(Genesis): resp_frame=%u game_frames=%u elapsed=%.1fs addrs=%d\n",
			g_md_state.last_resp_frame, g_md_state.game_frames, elapsed,
			ra_snes_addrlist_count());
	}
#endif
}

static int genesis_calculate_hash(const char *rom_path, char *md5_hex_out)
{
	(void)rom_path;
	(void)md5_hex_out;
	// Genesis uses standard MD5
	return 0; // Let main code handle it
}

static void genesis_set_hardcore(int enabled)
{
	if (enabled) {
		user_io_status_set("[64]", 1);  // Disable cheats
		user_io_status_set("[24]", 1);  // Disable save states
		ra_log_write("Genesis: Hardcore mode enabled\n");
	} else {
		user_io_status_set("[64]", 0);
		user_io_status_set("[24]", 0);
		ra_log_write("Genesis: Hardcore mode disabled\n");
	}
}

// Called from main achievements code to detect protocol
void genesis_detect_protocol(void *map)
{
	if (!map) return;
	const ra_header_t *hdr = (const ra_header_t *)map;
	if (hdr->region_count == 0) {
		g_md_state.optionc = 1;
		ra_log_write("Genesis FPGA protocol: Option C\n");
	}
}

// ---------------------------------------------------------------------------
// Console handler definition
// ---------------------------------------------------------------------------

const console_handler_t g_console_genesis = {
	.init = genesis_init,
	.reset = genesis_reset,
	.read_memory = genesis_read_memory,
	.poll = genesis_poll,
	.calculate_hash = genesis_calculate_hash,
	.set_hardcore = genesis_set_hardcore,
	.console_id = 1,  // RC_CONSOLE_MEGA_DRIVE
	.name = "Genesis"
};
