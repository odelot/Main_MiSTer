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
static int g_md_rtquery = 0;

// ---------------------------------------------------------------------------
// Genesis/MegaDrive Implementation
// ---------------------------------------------------------------------------

static void genesis_init(void)
{
	memset(&g_md_state, 0, sizeof(g_md_state));
	g_md_rtquery = 0;
}

static void genesis_reset(void)
{
	memset(&g_md_state, 0, sizeof(g_md_state));
	g_md_rtquery = 0;
}

static uint32_t genesis_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	if (g_md_state.seladdr) {
		if (g_md_state.collecting) {
			for (uint32_t i = 0; i < num_bytes; i++)
				ra_snes_addrlist_add(address + i);
		}
		if (g_md_state.cache_ready) {
			if (achievements_smart_cache_enabled() && g_md_rtquery) {
				// List-change misalignment is handled inside ra_snes_addrlist_*
				// (active-snapshot mapping) — no rtquery-all reindex needed.
				int any_miss = 0;
				for (uint32_t i = 0; i < num_bytes; i++) {
					if (ra_snes_addrlist_contains(address + i) < 0) {
						any_miss = 1; break;
					}
				}
				if (!any_miss) {
					for (uint32_t i = 0; i < num_bytes; i++)
						buffer[i] = ra_snes_addrlist_read_cached(map, address + i);
					return num_bytes;
				}
				if (num_bytes <= 4) {
					uint32_t val = ra_rtquery_read(map, address, num_bytes);
					for (uint32_t i = 0; i < num_bytes; i++) {
						buffer[i] = (uint8_t)(val >> (i * 8));
						ra_snes_addrlist_add_dynamic(address + i);
					}
					return num_bytes;
				}
				for (uint32_t i = 0; i < num_bytes; i++) {
					if (ra_snes_addrlist_contains(address + i) >= 0) {
						buffer[i] = ra_snes_addrlist_read_cached(map, address + i);
					} else {
						uint32_t val = ra_rtquery_read(map, address + i, 1);
						buffer[i] = (uint8_t)val;
						ra_snes_addrlist_add_dynamic(address + i);
					}
				}
				return num_bytes;
			}
			for (uint32_t i = 0; i < num_bytes; i++)
				buffer[i] = ra_snes_addrlist_read_cached(map, address + i);
			return num_bytes;
		}
		if (g_md_rtquery && achievements_rtquery_enabled() && !g_md_state.collecting && num_bytes <= 4) {
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

static int genesis_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
	if (!client || !game_loaded || !map || !g_md_state.seladdr) return 0;

	rc_client_t *rc_client = (rc_client_t *)client;

	// ===================================================================
	// Smart Cache path (Tier 1)
	// ===================================================================
	if (achievements_smart_cache_enabled() && g_md_rtquery) {

		if (ra_snes_addrlist_count() == 0 && !g_md_state.cache_ready) {
			// Re-prime to WAITING before the all-zero collection frame so a
			// mid-game re-bootstrap (stall recovery) cannot fire on zeros.
			rc_client_reset(rc_client);
			g_md_state.collecting = 1;
			ra_snes_addrlist_begin_collect();
			rc_client_do_frame(rc_client);
			g_md_state.collecting = 0;
			int changed = ra_snes_addrlist_end_collect(map);
			if (changed)
				ra_log_write("MD SmartCache: Bootstrap done, %d addrs\n", ra_snes_addrlist_count());
			else
				ra_log_write("MD SmartCache: No addresses collected\n");
		} else if (!g_md_state.cache_ready) {
			if (ra_snes_addrlist_is_ready(map)) {
				g_md_state.cache_ready = 1;
				g_md_state.last_resp_frame = 0;
				g_md_state.game_frames = 0;
				g_md_state.poll_logged = 0;
				clock_gettime(CLOCK_MONOTONIC, &g_md_state.cache_time);
				// Discard the zero-primed bootstrap state (delta conditions
				// would otherwise see 0 -> real transitions and fire).
				rc_client_reset(rc_client);
				ra_log_write("MD SmartCache: Cache active! %d addrs (rc_client reset)\n", ra_snes_addrlist_count());
			}
		} else {
			uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
			seladdr_resync_if_backward(&g_md_state, resp_frame, "Genesis");

			if (resp_frame > g_md_state.last_resp_frame) {
				g_md_state.last_resp_frame = resp_frame;
				g_md_state.game_frames++;
				ra_frame_processed(resp_frame);

				if (g_md_state.game_frames <= 5)
					ra_log_write("MD SmartCache: GameFrame %u (resp_frame=%u, addrs=%d)\n",
						g_md_state.game_frames, resp_frame, ra_snes_addrlist_count());

				// Evaluate only frames whose VALCACHE ordering is confirmed
				// (see seladdr_frame_evaluable).
				if (seladdr_frame_evaluable(map, "Genesis"))
					rc_client_do_frame(rc_client);

				// Dynamic-only prune (~1/min, only if dynamics piled up):
				// static bootstrap addresses stay; still-needed dynamics
				// re-add themselves via rtquery misses next frame.
				if (achievements_smart_cleanup_enabled()
						&& (g_md_state.game_frames % 3600 == 0)
						&& ra_snes_addrlist_dyn_count() > 128) {
					int removed = ra_snes_addrlist_prune_dynamic(map);
					if (removed) {
						ra_log_write("MD SmartCache: pruned %d dynamic addrs (%d static kept)\n",
							removed, ra_snes_addrlist_count());
					}
				} else if (ra_snes_addrlist_has_pending()) {
					ra_snes_addrlist_flush_dynamic(map);
				}
			}
		}

		uint32_t milestone = g_md_state.game_frames / 300;
		if (milestone > 0 && milestone != g_md_state.poll_logged) {
			g_md_state.poll_logged = milestone;
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double elapsed = (now.tv_sec - g_md_state.cache_time.tv_sec)
				+ (now.tv_nsec - g_md_state.cache_time.tv_nsec) / 1e9;
			double ms_per_cycle = (g_md_state.game_frames > 0) ?
				(elapsed * 1000.0 / g_md_state.game_frames) : 0.0;
			ra_log_write("POLL(MD-SC): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d\n",
				g_md_state.last_resp_frame, g_md_state.game_frames, elapsed, ms_per_cycle,
				ra_snes_addrlist_count());
		}
		return 1;
	}

	// ===================================================================
	// Legacy path
	// ===================================================================

	if (ra_snes_addrlist_count() == 0 && !g_md_state.cache_ready) {
		// Bootstrap: run one do_frame with zeros to discover needed addresses.
		// Re-prime to WAITING first (see smart-cache bootstrap).
		rc_client_reset(rc_client);
		g_md_state.collecting = 1;
		ra_snes_addrlist_begin_collect();
		rc_client_do_frame(rc_client);
		g_md_state.collecting = 0;
		int changed = ra_snes_addrlist_end_collect(map);
		if (changed) {
			ra_log_write("MD SelAddr: Bootstrap collection done, %d addrs written to DDRAM\n",
				ra_snes_addrlist_count());
		} else {
			ra_log_write("MD SelAddr: No addresses collected\n");
		}
	} else if (!g_md_state.cache_ready) {
		// Wait for FPGA to respond with cached values
		if (ra_snes_addrlist_is_ready(map)) {
			g_md_state.cache_ready = 1;
			g_md_state.last_resp_frame = 0;
			g_md_state.game_frames = 0;
			g_md_state.poll_logged = 0;
			clock_gettime(CLOCK_MONOTONIC, &g_md_state.cache_time);
			// Discard the zero-primed bootstrap state (see smart-cache path).
			rc_client_reset(rc_client);
			ra_log_write("MD SelAddr: Cache active! FPGA response matched request (rc_client reset).\n");
			// Dump address list once on activation
			const uint32_t *a0 = ra_snes_addrlist_addrs();
			int ac = ra_snes_addrlist_count();
			int ad = ac < 20 ? ac : 20;
			char ah[256]; int ap = 0;
			for (int i = 0; i < ad && ap < (int)sizeof(ah) - 8; i++)
				ap += snprintf(ah + ap, sizeof(ah) - ap, "%04X ", a0[i]);
			ra_log_write("MD ADDRLIST[0..%d]: %s (total=%d)\n", ad - 1, ah, ac);
		}
	} else {
		// Normal frame processing from cache
		uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
		seladdr_resync_if_backward(&g_md_state, resp_frame, "Genesis");
		if (resp_frame > g_md_state.last_resp_frame) {
			g_md_state.last_resp_frame = resp_frame;
			g_md_state.game_frames++;
			ra_frame_processed(resp_frame);
			clock_gettime(CLOCK_MONOTONIC, &g_md_state.stall_time);
			g_md_state.stall_frame = resp_frame;

			// Early-frame valcache dump (first 5 frames)
			if (g_md_state.game_frames <= 5) {
				const uint8_t *ev = (const uint8_t *)map + 0x48000 + 8;
				int ec = ra_snes_addrlist_count();
				int ed = ec < 45 ? ec : 45;
				int enz = 0;
				char eh[256]; int ep = 0;
				for (int i = 0; i < ed && ep < (int)sizeof(eh) - 4; i++) {
					ep += snprintf(eh + ep, sizeof(eh) - ep, "%02X ", ev[i]);
					if (ev[i]) enz++;
				}
				ra_log_write("MD early[%u]: VALCACHE %s (%d nz)\n",
					g_md_state.game_frames, eh, enz);
			}

			// Skip achievement processing while a recollect revision is in
			// flight (newly collected addresses would read as 0).
			if (ra_snes_addrlist_is_ready(map)) {
				// Re-collect every ~5 min to catch address changes
				// Smart cache mode: skip re-collect (no dynamic pointers in Genesis)
				int re_collect = !achievements_smart_cache_enabled()
					&& (g_md_state.game_frames % 18000 == 0) && (g_md_state.game_frames > 0);
				if (re_collect) {
					g_md_state.collecting = 1;
					ra_snes_addrlist_begin_collect();
				}

				rc_client_do_frame(rc_client);

				if (re_collect) {
					g_md_state.collecting = 0;
					if (ra_snes_addrlist_end_collect(map)) {
						ra_log_write("MD SelAddr: Address list refreshed, %d addrs\n",
							ra_snes_addrlist_count());
					}
				}
			}
		} else {
			seladdr_check_stall_recovery(&g_md_state, resp_frame, "Genesis");
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
		double ms_per_cycle = (g_md_state.game_frames > 0) ?
			(elapsed * 1000.0 / g_md_state.game_frames) : 0.0;
		ra_log_write("POLL(MD): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d\n",
			g_md_state.last_resp_frame, g_md_state.game_frames, elapsed, ms_per_cycle,
			ra_snes_addrlist_count());
	}

	return 1; // MegaDrive handled
#else
	return 0;
#endif
}

static int genesis_calculate_hash(const char *rom_path, char *md5_hex_out)
{
	(void)rom_path;
	(void)md5_hex_out;
	return 0; // use default MD5
}

static void genesis_set_hardcore(int enabled)
{
	// bit 64: hardcore signal -- the core gates both cheat engines (68k + Z80)
	// with ~hardcore and hides the Cheats page. Only this bit is touched:
	// status[24] is the user's "Cheats Enabled" OSD toggle (not save states),
	// and writing it forced "Yes" on every core/game load when hardcore was off.
	// No restore-state to block: the MegaDrive core has no save states (no SS
	// in CONF_STR). If upstream ever adds them, gate restore with ~hardcore in
	// the core like SNES/NES/GB do.
	user_io_status_set("[64]", enabled ? 1 : 0);
	ra_log_write("Genesis: Hardcore mode %s\n", enabled ? "enabled" : "disabled");
}

static int genesis_detect_protocol(void *map)
{
	if (!ra_ramread_active(map)) {
		ra_log_write("Genesis: FPGA mirror not detected -- RA support unavailable\n");
		return 0;
	}
	g_md_state.seladdr = 1;
	ra_log_write("MegaDrive FPGA protocol: Selective Address (selective address reading)\n");

	if (ra_rtquery_supported(map) && achievements_rtquery_enabled()) {
		g_md_rtquery = 1;
		ra_rtquery_init(map);
		ra_log_write("Genesis: Realtime queries supported and ENABLED\n");
		// Lightgun A/B test (retroachievements.cfg: justifier_test=1): keep
		// the smart cache but cap the rtquery busy-wait to ~2k iterations
		// (~0.5-1ms worst case, plus a 20ms fail-fast cooldown on timeout)
		// so a mailbox stall can never delay mouse->SPI input forwarding.
		if (achievements_justifier_test()) {
			ra_rtquery_set_spin_limit(2000);
			ra_log_write("Genesis: JUSTIFIER TEST active -- rtquery spin capped\n");
		}
	} else if (ra_rtquery_supported(map)) {
		g_md_rtquery = 0;
		ra_log_write("Genesis: Realtime queries supported but DISABLED by config\n");
	} else {
		g_md_rtquery = 0;
		ra_log_write("Genesis: Realtime queries NOT supported (FPGA v1)\n");
	}
	return 1;
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
	.detect_protocol = genesis_detect_protocol,
	.console_id = 1,  // RC_CONSOLE_MEGA_DRIVE
	.name = "Genesis",
	.hardcore_protected = 1
};
