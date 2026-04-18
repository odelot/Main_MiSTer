// achievements_gameboy.cpp — RetroAchievements GameBoy/GameBoy Color-specific implementation

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
// GameBoy State
// ---------------------------------------------------------------------------

static console_state_t g_gb_state = {};

// Value-change watchers for debugging
typedef struct {
	uint32_t addr;
	uint8_t last_val;
	int initialized;
	const char *name;
} gb_watcher_t;

static gb_watcher_t g_gb_watchers[] = {
	{0xD190, 0, 0, "stateChange"},
	{0xFF99, 0, 0, "gameMode"},
	{0xFFFA, 0, 0, "coins"},
	{0xFFB3, 0, 0, "gameState"},
	{0xFFB4, 0, 0, "subState"},
	{0xFFDA, 0, 0, "secState"},
	{0xC0A0, 0, 0, "scoreHi"},
	{0xC0A1, 0, 0, "scoreLo"},
};
#define GB_WATCHER_COUNT (sizeof(g_gb_watchers) / sizeof(g_gb_watchers[0]))

// ---------------------------------------------------------------------------
// GameBoy Implementation
// ---------------------------------------------------------------------------

static void gameboy_init(void)
{
	memset(&g_gb_state, 0, sizeof(g_gb_state));
	memset(g_gb_watchers, 0, sizeof(g_gb_watchers));
	ra_snes_addrlist_init();
}

static void gameboy_reset(void)
{
	g_gb_state.optionc = 0;
	g_gb_state.collecting = 0;
	g_gb_state.cache_ready = 0;
	g_gb_state.last_resp_frame = 0;
	g_gb_state.game_frames = 0;
	g_gb_state.poll_logged = 0;
	
	// Reset watchers
	for (unsigned int i = 0; i < GB_WATCHER_COUNT; i++) {
		g_gb_watchers[i].initialized = 0;
		g_gb_watchers[i].last_val = 0;
	}
}

static uint32_t gameboy_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	if (g_gb_state.optionc) {
		if (g_gb_state.collecting) {
			for (uint32_t i = 0; i < num_bytes; i++)
				ra_snes_addrlist_add(address + i);
		}
		if (g_gb_state.cache_ready) {
			for (uint32_t i = 0; i < num_bytes; i++)
				buffer[i] = ra_snes_addrlist_read_cached(map, address + i);
			return num_bytes;
		}
		memset(buffer, 0, num_bytes);
		return num_bytes;
	}
	return 0;
}

static void gameboy_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
	if (!client || !game_loaded || !map || !g_gb_state.optionc) return;
	
	rc_client_t *rc_client = (rc_client_t *)client;

	if (ra_snes_addrlist_count() == 0 && !g_gb_state.cache_ready) {
		// Bootstrap
		g_gb_state.collecting = 1;
		ra_snes_addrlist_begin_collect();
		rc_client_do_frame(rc_client);
		g_gb_state.collecting = 0;
		int changed = ra_snes_addrlist_end_collect(map);
		if (changed) {
			ra_log_write("GameBoy OptionC: Bootstrap collection done, %d addrs\n",
				ra_snes_addrlist_count());
		}
	} else if (!g_gb_state.cache_ready) {
		// Wait for cache
		if (ra_snes_addrlist_is_ready(map)) {
			g_gb_state.cache_ready = 1;
			g_gb_state.last_resp_frame = 0;
			g_gb_state.game_frames = 0;
			g_gb_state.poll_logged = 0;
			clock_gettime(CLOCK_MONOTONIC, &g_gb_state.cache_time);
			ra_log_write("GameBoy OptionC: Cache active!\n");
		}
	} else {
		// Normal processing
		uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
		if (resp_frame > g_gb_state.last_resp_frame) {
			g_gb_state.last_resp_frame = resp_frame;
			g_gb_state.game_frames++;

			// Initialize watchers on first frame
			if (g_gb_state.game_frames == 1) {
				for (unsigned int i = 0; i < GB_WATCHER_COUNT; i++) {
					g_gb_watchers[i].last_val = ra_snes_addrlist_read_cached(map, g_gb_watchers[i].addr);
					g_gb_watchers[i].initialized = 1;
				}
			}

			// Check for value changes in watchers
			for (unsigned int i = 0; i < GB_WATCHER_COUNT; i++) {
				if (!g_gb_watchers[i].initialized) continue;
				uint8_t cur_val = ra_snes_addrlist_read_cached(map, g_gb_watchers[i].addr);
				if (cur_val != g_gb_watchers[i].last_val) {
					ra_log_write("GB Watch[%s @ 0x%04X]: 0x%02X -> 0x%02X (frame %u)\n",
						g_gb_watchers[i].name, g_gb_watchers[i].addr,
						g_gb_watchers[i].last_val, cur_val, g_gb_state.game_frames);
					g_gb_watchers[i].last_val = cur_val;
				}
			}

			// Re-collect every ~5 min
			int re_collect = (g_gb_state.game_frames % 18000 == 0) && (g_gb_state.game_frames > 0);
			if (re_collect) {
				g_gb_state.collecting = 1;
				ra_snes_addrlist_begin_collect();
			}

			rc_client_do_frame(rc_client);

			if (re_collect) {
				g_gb_state.collecting = 0;
				if (ra_snes_addrlist_end_collect(map)) {
					ra_log_write("GameBoy OptionC: Address list refreshed, %d addrs\n",
						ra_snes_addrlist_count());
				}
			}
		}
	}

	// Periodic log
	uint32_t milestone = g_gb_state.game_frames / 300;
	if (milestone > 0 && milestone != g_gb_state.poll_logged) {
		g_gb_state.poll_logged = milestone;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = (now.tv_sec - g_gb_state.cache_time.tv_sec)
			+ (now.tv_nsec - g_gb_state.cache_time.tv_nsec) / 1e9;
		ra_log_write("POLL(GB): resp_frame=%u game_frames=%u elapsed=%.1fs addrs=%d\n",
			g_gb_state.last_resp_frame, g_gb_state.game_frames, elapsed,
			ra_snes_addrlist_count());
	}
#endif
}

static int gameboy_calculate_hash(const char *rom_path, char *md5_hex_out)
{
	(void)rom_path;
	(void)md5_hex_out;
	// GameBoy uses standard MD5
	return 0; // Let main code handle it
}

static void gameboy_set_hardcore(int enabled)
{
	ra_log_write("GameBoy: Hardcore mode %s (no FPGA bits mapped yet)\n",
		enabled ? "enabled" : "disabled");
}

// Called from main achievements code to detect protocol
void gameboy_detect_protocol(void *map)
{
	if (!map) return;
	const ra_header_t *hdr = (const ra_header_t *)map;
	if (hdr->region_count == 0) {
		g_gb_state.optionc = 1;
		ra_log_write("GameBoy FPGA protocol: Option C (selective address reading)\n");
	}
}

// ---------------------------------------------------------------------------
// Console handler definition
// ---------------------------------------------------------------------------

const console_handler_t g_console_gameboy = {
	.init = gameboy_init,
	.reset = gameboy_reset,
	.read_memory = gameboy_read_memory,
	.poll = gameboy_poll,
	.calculate_hash = gameboy_calculate_hash,
	.set_hardcore = gameboy_set_hardcore,
	.console_id = 4,  // RC_CONSOLE_GAMEBOY (also handles GBC with ID 6)
	.name = "GAMEBOY"
};
