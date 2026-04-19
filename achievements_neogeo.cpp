// achievements_neogeo.cpp — RetroAchievements NeoGeo (MVS/CD) implementation

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
static int g_neogeo_is_cd = 0; // 1 = NeoGeoCD (no byte-swap), 0 = MVS (XOR addr^1)

// ---------------------------------------------------------------------------
// NeoGeo Implementation
// ---------------------------------------------------------------------------

static void neogeo_init(void)
{
	memset(&g_neogeo_state, 0, sizeof(g_neogeo_state));
	g_neogeo_is_cd = 0;
}

static void neogeo_reset(void)
{
	memset(&g_neogeo_state, 0, sizeof(g_neogeo_state));
	g_neogeo_is_cd = 0;
}

static uint32_t neogeo_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	if (g_neogeo_state.optionc) {
		if (g_neogeo_state.collecting) {
			for (uint32_t i = 0; i < num_bytes; i++) {
				// NeoGeoCD: natural 68K big-endian order, no XOR needed.
				// NeoGeo MVS (fbneo): 16-bit words in host little-endian order, XOR addr bit 0.
				uint32_t a = g_neogeo_is_cd ? (address + i) : ((address + i) ^ 1);
				ra_snes_addrlist_add(a);
			}
		}
		if (g_neogeo_state.cache_ready) {
			for (uint32_t i = 0; i < num_bytes; i++) {
				uint32_t a = g_neogeo_is_cd ? (address + i) : ((address + i) ^ 1);
				buffer[i] = ra_snes_addrlist_read_cached(map, a);
			}
			return num_bytes;
		}
		memset(buffer, 0, num_bytes);
		return num_bytes;
	}
	return 0;
}

static int neogeo_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
	if (!client || !game_loaded || !map || !g_neogeo_state.optionc) return 0;

	rc_client_t *rc_client = (rc_client_t *)client;

	if (ra_snes_addrlist_count() == 0 && !g_neogeo_state.cache_ready) {
		// Phase 1: Bootstrap — collect addresses with zeros
		g_neogeo_state.collecting = 1;
		ra_snes_addrlist_begin_collect();
		rc_client_do_frame(rc_client);
		g_neogeo_state.collecting = 0;
		int changed = ra_snes_addrlist_end_collect(map);
		if (changed) {
			g_neogeo_state.needs_recollect = 1;
			g_neogeo_state.resolve_pass = 0;
			ra_log_write("NeoGeo OptionC: Bootstrap collection done, %d addrs written to DDRAM\n",
				ra_snes_addrlist_count());
		} else {
			ra_log_write("NeoGeo OptionC: No addresses collected\n");
		}
	} else if (!g_neogeo_state.cache_ready) {
		// Phase 2/4: Wait for FPGA cache
		if (ra_snes_addrlist_is_ready(map)) {
			g_neogeo_state.cache_ready = 1;
			g_neogeo_state.last_resp_frame = 0;
			g_neogeo_state.game_frames = 0;
			g_neogeo_state.poll_logged = 0;
			clock_gettime(CLOCK_MONOTONIC, &g_neogeo_state.cache_time);
			if (g_neogeo_state.needs_recollect) {
				ra_log_write("NeoGeo OptionC: Cache active (pre-recollect). Will resolve pointers.\n");
			} else {
				ra_log_write("NeoGeo OptionC: Cache active! FPGA response matched request.\n");
			}
			const uint32_t *a0 = ra_snes_addrlist_addrs();
			int ac = ra_snes_addrlist_count();
			int ad = ac < 20 ? ac : 20;
			char ah[256]; int ap = 0;
			for (int i = 0; i < ad && ap < (int)sizeof(ah) - 8; i++)
				ap += snprintf(ah + ap, sizeof(ah) - ap, "%04X ", a0[i]);
			ra_log_write("NeoGeo ADDRLIST[0..%d]: %s (total=%d)\n", ad - 1, ah, ac);
		}
	} else if (g_neogeo_state.needs_recollect) {
		// Phase 3: Pointer-resolution re-collection
		g_neogeo_state.resolve_pass++;
		g_neogeo_state.collecting = 1;
		ra_snes_addrlist_begin_collect();
		rc_client_do_frame(rc_client);
		g_neogeo_state.collecting = 0;
		int changed = ra_snes_addrlist_end_collect(map);
		if (changed && g_neogeo_state.resolve_pass < 4) {
			ra_log_write("NeoGeo OptionC: Pointer-resolve pass %d, %d addrs (changed, retrying)\n",
				g_neogeo_state.resolve_pass, ra_snes_addrlist_count());
			g_neogeo_state.cache_ready = 0; // wait for new FPGA data
		} else {
			g_neogeo_state.needs_recollect = 0;
			if (changed) {
				ra_log_write("NeoGeo OptionC: Pointer-resolve done (max passes), %d addrs\n",
					ra_snes_addrlist_count());
				g_neogeo_state.cache_ready = 0;
			} else {
				ra_log_write("NeoGeo OptionC: Pointer-resolve complete, no address changes (%d addrs)\n",
					ra_snes_addrlist_count());
			}
		}
	} else {
		// Phase 5: Normal frame processing from cache
		uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
		if (resp_frame > g_neogeo_state.last_resp_frame) {
			g_neogeo_state.last_resp_frame = resp_frame;
			g_neogeo_state.game_frames++;
			ra_frame_processed(resp_frame);

			// Re-collect every 18000 frames (~5min).
			// Do NOT invalidate cache: avoids zeros during active gameplay.
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

	uint32_t milestone = g_neogeo_state.game_frames / 300;
	if (milestone > 0 && milestone != g_neogeo_state.poll_logged) {
		g_neogeo_state.poll_logged = milestone;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = (now.tv_sec - g_neogeo_state.cache_time.tv_sec)
			+ (now.tv_nsec - g_neogeo_state.cache_time.tv_nsec) / 1e9;
		double ms_per_cycle = (g_neogeo_state.game_frames > 0) ?
			(elapsed * 1000.0 / g_neogeo_state.game_frames) : 0.0;
		ra_log_write("POLL(NeoGeo%s): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d\n",
			g_neogeo_is_cd ? "CD" : "",
			g_neogeo_state.last_resp_frame, g_neogeo_state.game_frames, elapsed, ms_per_cycle,
			ra_snes_addrlist_count());
	}

	return 1; // NeoGeo handled
#else
	return 0;
#endif
}

static int neogeo_calculate_hash(const char *rom_path, char *md5_hex_out)
{
#ifdef HAS_RCHEEVOS
	char abs_path[1024];
	if (rom_path[0] == '/') {
		snprintf(abs_path, sizeof(abs_path), "%s", rom_path);
	} else {
		extern const char *getRootDir(void);
		snprintf(abs_path, sizeof(abs_path), "%s/%s", getRootDir(), rom_path);
	}

	// NeoGeoCD uses console_id=56, MVS uses console_id=27
	int cid = g_neogeo_is_cd ? 56 : 27;
	const char *cname = g_neogeo_is_cd ? "NeoGeoCD" : "NeoGeo";

	if (rc_hash_generate_from_file(md5_hex_out, cid, abs_path)) {
		ra_log_write("%s hash: %s\n", cname, md5_hex_out);
		return 1;
	}
	ra_log_write("%s: rc_hash_generate_from_file failed\n", cname);
#endif
	return 0;
}

static void neogeo_set_hardcore(int enabled)
{
	ra_log_write("NeoGeo: Hardcore mode %s (no FPGA bits mapped)\n",
		enabled ? "enabled" : "disabled");
}

static void neogeo_detect_protocol(void *map)
{
	(void)map;
	// NeoGeo always uses Option C; detect CD vs MVS for byte-ordering
	g_neogeo_state.optionc = 1;
	g_neogeo_is_cd = is_neogeo_cd();
	ra_log_write("%s FPGA protocol: Option C (selective address reading)\n",
		g_neogeo_is_cd ? "NeoGeoCD" : "NeoGeo");
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
	.detect_protocol = neogeo_detect_protocol,
	.console_id = 27,  // RC_CONSOLE_ARCADE (also handles NeoGeoCD with ID 56)
	.name = "NEOGEO"
};
