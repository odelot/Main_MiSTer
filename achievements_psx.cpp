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
static int g_psx_rtquery = 0; // 1 if FPGA supports realtime queries


// ---------------------------------------------------------------------------
// PSX Option C Diagnostics
// ---------------------------------------------------------------------------

static void psx_optionc_dump_valcache(const char *label, void *map)
{
	if (!map) return;
	const uint8_t *base = (const uint8_t *)map;
	int addr_count = ra_snes_addrlist_count();
	const ra_val_resp_hdr_t *resp = (const ra_val_resp_hdr_t *)(base + RA_SNES_VALCACHE_OFFSET);
	const uint8_t *vals = base + RA_SNES_VALCACHE_OFFSET + 8;

	int dump_len = addr_count < 45 ? addr_count : 45;
	int non_zero = 0;
	char hex[256]; int pos = 0;
	for (int i = 0; i < dump_len && pos < (int)sizeof(hex) - 4; i++) {
		pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", vals[i]);
		if (vals[i]) non_zero++;
	}
	ra_log_write("PSX DUMP[%s] resp_id=%u resp_frame=%u addrs=%d non_zero=%d\n",
		label, resp->response_id, resp->response_frame, addr_count, non_zero);
	ra_log_write("PSX DUMP[%s] VALCACHE[0..%d]: %s\n", label, dump_len - 1, hex);

	const uint32_t *addrs = ra_snes_addrlist_addrs();
	int show = addr_count < 6 ? addr_count : 6;
	for (int i = 0; i < show; i++)
		ra_log_write("PSX DUMP[%s]   [%d] addr=0x%06X val=0x%02X\n", label, i, addrs[i], vals[i]);
}

// ---------------------------------------------------------------------------
// PSX Implementation
// ---------------------------------------------------------------------------

static void psx_init(void)
{
	memset(&g_psx_state, 0, sizeof(g_psx_state));
	g_psx_rtquery = 0;
}

static void psx_reset(void)
{
	memset(&g_psx_state, 0, sizeof(g_psx_state));
	g_psx_rtquery = 0;
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
		// Realtime query fallback for addresses not in batch cache
		if (g_psx_rtquery && !g_psx_state.collecting && num_bytes <= 4) {
			uint32_t val = ra_rtquery_read(map, address, num_bytes);
			for (uint32_t i = 0; i < num_bytes; i++)
				buffer[i] = (uint8_t)(val >> (i * 8));
			return num_bytes;
		}
		memset(buffer, 0, num_bytes);
		return num_bytes;
	}
	return 0;
}

static int psx_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
	if (!client || !game_loaded || !map || !g_psx_state.optionc) return 0;

	rc_client_t *rc_client = (rc_client_t *)client;

	if (ra_snes_addrlist_count() == 0 && !g_psx_state.cache_ready) {
		// Phase 1: Bootstrap — collect addresses with zero values
		g_psx_state.collecting = 1;
		ra_snes_addrlist_begin_collect();
		rc_client_do_frame(rc_client);
		g_psx_state.collecting = 0;
		int changed = ra_snes_addrlist_end_collect(map);
		if (changed) {
			g_psx_state.needs_recollect = 1;
			ra_log_write("PSX OptionC: Bootstrap collection done, %d addrs written to DDRAM\n",
				ra_snes_addrlist_count());
			psx_optionc_dump_valcache("bootstrap", map);
		} else {
			ra_log_write("PSX OptionC: No addresses collected\n");
		}
	} else if (!g_psx_state.cache_ready) {
		// Phase 2/4: Wait for FPGA cache
		if (ra_snes_addrlist_is_ready(map)) {
			g_psx_state.cache_ready = 1;
			g_psx_state.last_resp_frame = 0;
			g_psx_state.game_frames = 0;
			g_psx_state.poll_logged = 0;
			clock_gettime(CLOCK_MONOTONIC, &g_psx_state.cache_time);
			if (g_psx_state.needs_recollect) {
				ra_log_write("PSX OptionC: Cache active (pre-recollect). Will resolve pointers.\n");
				psx_optionc_dump_valcache("pre-recollect", map);
			} else {
				ra_log_write("PSX OptionC: Cache active! FPGA response matched request.\n");
				psx_optionc_dump_valcache("cache-active", map);
			}
		}
	} else if (g_psx_state.needs_recollect) {
		// Phase 3: Pointer-resolution re-collection
		// Now that cache has real values, AddAddress conditions will
		// compute correct derived addresses from pointer base values.
		g_psx_state.needs_recollect = 0;
		g_psx_state.collecting = 1;
		ra_snes_addrlist_begin_collect();
		rc_client_do_frame(rc_client);
		g_psx_state.collecting = 0;
		int changed = ra_snes_addrlist_end_collect(map);
		if (changed) {
			ra_log_write("PSX OptionC: Pointer-resolve re-collection done, %d addrs (changed)\n",
				ra_snes_addrlist_count());
			g_psx_state.cache_ready = 0; // wait for new FPGA data with correct addresses
			psx_optionc_dump_valcache("ptr-resolve", map);
		} else {
			ra_log_write("PSX OptionC: Pointer-resolve complete, no address changes\n");
		}
	} else {
		// Phase 5: Normal frame processing
		uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
		if (resp_frame > g_psx_state.last_resp_frame) {
			g_psx_state.last_resp_frame = resp_frame;
			g_psx_state.game_frames++;
			ra_frame_processed(resp_frame);
			clock_gettime(CLOCK_MONOTONIC, &g_psx_state.stall_time);
			g_psx_state.stall_frame = resp_frame;

			if (g_psx_state.game_frames <= 5) {
				ra_log_write("PSX OptionC: GameFrame %u (resp_frame=%u)\n",
					g_psx_state.game_frames, resp_frame);
				psx_optionc_dump_valcache("early-frame", map);
			}

			// Re-collect every 600 frames (~10s) to track pointer changes
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
					g_psx_state.cache_ready = 0; // wait for new FPGA data
				}
			}
		} else {
			optionc_check_stall_recovery(&g_psx_state, resp_frame, "PSX");
		}
	}

	uint32_t milestone = g_psx_state.game_frames / 300;
	if (milestone > 0 && milestone != g_psx_state.poll_logged) {
		g_psx_state.poll_logged = milestone;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = (now.tv_sec - g_psx_state.cache_time.tv_sec)
			+ (now.tv_nsec - g_psx_state.cache_time.tv_nsec) / 1e9;
		double ms_per_cycle = (g_psx_state.game_frames > 0) ?
			(elapsed * 1000.0 / g_psx_state.game_frames) : 0.0;
		ra_log_write("POLL(PSX): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d\n",
			g_psx_state.last_resp_frame, g_psx_state.game_frames, elapsed, ms_per_cycle,
			ra_snes_addrlist_count());
		if ((g_psx_state.game_frames % 1800) < 300)
			psx_optionc_dump_valcache("periodic", map);
	}

	return 1; // PSX handled
#else
	return 0;
#endif
}

static int psx_calculate_hash(const char *rom_path, char *md5_hex_out)
{
#ifdef HAS_RCHEEVOS
	char abs_path[1024];
	if (rom_path[0] == '/') {
		snprintf(abs_path, sizeof(abs_path), "%s", rom_path);
	} else {
		extern const char *getRootDir(void);
		snprintf(abs_path, sizeof(abs_path), "%s/%s", getRootDir(), rom_path);
	}

	if (rc_hash_generate_from_file(md5_hex_out, 12, abs_path)) {
		ra_log_write("PSX hash: %s\n", md5_hex_out);
		return 1;
	}
	ra_log_write("PSX: rc_hash_generate_from_file failed for %s\n", abs_path);
#endif
	return 0;
}

static void psx_set_hardcore(int enabled)
{
	user_io_status_set("[93]", enabled ? 1 : 0); // disable cheats
	user_io_status_set("[6]",  enabled ? 1 : 0); // disable save states
	ra_log_write("PSX: Hardcore mode %s\n", enabled ? "enabled" : "disabled");
}

static int psx_detect_protocol(void *map)
{
	if (!ra_ramread_active(map)) {
		ra_log_write("PSX: FPGA mirror not detected -- RA support unavailable\n");
		return 0;
	}
	// PSX always uses Option C (no VBlank-gated mode)
	g_psx_state.optionc = 1;
	ra_log_write("PSX FPGA protocol: Option C (selective address reading)\n");

	if (ra_rtquery_supported(map)) {
		g_psx_rtquery = 1;
		ra_rtquery_init(map);
		ra_log_write("PSX: Realtime queries supported (FPGA v2+)\n");
	} else {
		g_psx_rtquery = 0;
		ra_log_write("PSX: Realtime queries NOT supported (FPGA v1)\n");
	}
	return 1;
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
	.detect_protocol = psx_detect_protocol,
	.console_id = 12,  // RC_CONSOLE_PLAYSTATION
	.name = "PSX"
};
