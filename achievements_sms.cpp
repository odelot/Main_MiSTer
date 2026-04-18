// achievements_sms.cpp — RetroAchievements Master System/Game Gear-specific implementation

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
// SMS State
// ---------------------------------------------------------------------------

static console_state_t g_sms_state = {};

// ---------------------------------------------------------------------------
// SMS Implementation
// ---------------------------------------------------------------------------

static void sms_init(void)
{
	memset(&g_sms_state, 0, sizeof(g_sms_state));
	ra_snes_addrlist_init();
}

static void sms_reset(void)
{
	g_sms_state.optionc = 0;
	g_sms_state.collecting = 0;
	g_sms_state.cache_ready = 0;
	g_sms_state.last_resp_frame = 0;
	g_sms_state.game_frames = 0;
	g_sms_state.poll_logged = 0;
}

static uint32_t sms_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	if (g_sms_state.optionc) {
		if (g_sms_state.collecting) {
			for (uint32_t i = 0; i < num_bytes; i++)
				ra_snes_addrlist_add(address + i);
		}
		if (g_sms_state.cache_ready) {
			for (uint32_t i = 0; i < num_bytes; i++)
				buffer[i] = ra_snes_addrlist_read_cached(map, address + i);
			return num_bytes;
		}
		memset(buffer, 0, num_bytes);
		return num_bytes;
	}
	return 0;
}

static void sms_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
	if (!client || !game_loaded || !map || !g_sms_state.optionc) return;
	
	rc_client_t *rc_client = (rc_client_t *)client;

	if (ra_snes_addrlist_count() == 0 && !g_sms_state.cache_ready) {
		// Bootstrap
		g_sms_state.collecting = 1;
		ra_snes_addrlist_begin_collect();
		rc_client_do_frame(rc_client);
		g_sms_state.collecting = 0;
		int changed = ra_snes_addrlist_end_collect(map);
		if (changed) {
			ra_log_write("SMS OptionC: Bootstrap collection done, %d addrs\n",
				ra_snes_addrlist_count());
		}
	} else if (!g_sms_state.cache_ready) {
		// Wait for cache
		if (ra_snes_addrlist_is_ready(map)) {
			g_sms_state.cache_ready = 1;
			g_sms_state.last_resp_frame = 0;
			g_sms_state.game_frames = 0;
			g_sms_state.poll_logged = 0;
			clock_gettime(CLOCK_MONOTONIC, &g_sms_state.cache_time);
			ra_log_write("SMS OptionC: Cache active!\n");
		}
	} else {
		// Normal processing
		uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
		if (resp_frame > g_sms_state.last_resp_frame) {
			g_sms_state.last_resp_frame = resp_frame;
			g_sms_state.game_frames++;

			// Re-collect every ~5 min
			int re_collect = (g_sms_state.game_frames % 18000 == 0) && (g_sms_state.game_frames > 0);
			if (re_collect) {
				g_sms_state.collecting = 1;
				ra_snes_addrlist_begin_collect();
			}

			rc_client_do_frame(rc_client);

			if (re_collect) {
				g_sms_state.collecting = 0;
				if (ra_snes_addrlist_end_collect(map)) {
					ra_log_write("SMS OptionC: Address list refreshed, %d addrs\n",
						ra_snes_addrlist_count());
				}
			}
		}
	}

	// Periodic log
	uint32_t milestone = g_sms_state.game_frames / 300;
	if (milestone > 0 && milestone != g_sms_state.poll_logged) {
		g_sms_state.poll_logged = milestone;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = (now.tv_sec - g_sms_state.cache_time.tv_sec)
			+ (now.tv_nsec - g_sms_state.cache_time.tv_nsec) / 1e9;
		ra_log_write("POLL(SMS): resp_frame=%u game_frames=%u elapsed=%.1fs addrs=%d\n",
			g_sms_state.last_resp_frame, g_sms_state.game_frames, elapsed,
			ra_snes_addrlist_count());
	}
#endif
}

static int sms_calculate_hash(const char *rom_path, char *md5_hex_out)
{
	(void)rom_path;
	(void)md5_hex_out;
	// SMS uses standard MD5
	return 0; // Let main code handle it
}

static void sms_set_hardcore(int enabled)
{
	if (enabled) {
		user_io_status_set("[24]", 1);  // Disable cheats
		ra_log_write("SMS: Hardcore mode enabled (cheats disabled)\n");
	} else {
		user_io_status_set("[24]", 0);
		ra_log_write("SMS: Hardcore mode disabled\n");
	}
}

// Called from main achievements code to detect protocol
void sms_detect_protocol(void *map)
{
	if (!map) return;
	const ra_header_t *hdr = (const ra_header_t *)map;
	if (hdr->region_count == 0) {
		g_sms_state.optionc = 1;
		ra_log_write("SMS FPGA protocol: Option C\n");
	}
}

// ---------------------------------------------------------------------------
// Console handler definition
// ---------------------------------------------------------------------------

const console_handler_t g_console_sms = {
	.init = sms_init,
	.reset = sms_reset,
	.read_memory = sms_read_memory,
	.poll = sms_poll,
	.calculate_hash = sms_calculate_hash,
	.set_hardcore = sms_set_hardcore,
	.console_id = 11,  // RC_CONSOLE_MASTER_SYSTEM (also handles Game Gear ID 15)
	.name = "SMS"
};
