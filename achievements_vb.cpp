// achievements_vb.cpp — RetroAchievements Virtual Boy-specific implementation
//
// rcheevos address map (RC_CONSOLE_VIRTUAL_BOY):
//   0x00000-0x0FFFF = System RAM / WRAM (64KB, phys 0x05000000)
//   0x10000-0x1FFFF = Cartridge RAM     (64KB window, phys 0x06000000)
//
// The FPGA mirror (ra_ram_mirror_vb v0x02) serves WRAM from a dedicated
// BRAM port and cartridge RAM from its coherent DDR3 shadow, with the
// full Selective Address + rtquery mailbox protocol.

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
// Virtual Boy State
// ---------------------------------------------------------------------------

static console_state_t g_vb_state = {};
static int g_vb_rtquery = 0;

// ---------------------------------------------------------------------------
// Virtual Boy Implementation
// ---------------------------------------------------------------------------

static void vb_init(void)
{
	memset(&g_vb_state, 0, sizeof(g_vb_state));
	g_vb_rtquery = 0;
}

static void vb_reset(void)
{
	memset(&g_vb_state, 0, sizeof(g_vb_state));
	g_vb_rtquery = 0;
}

static uint32_t vb_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	if (g_vb_state.seladdr) {
		if (g_vb_state.collecting) {
			for (uint32_t i = 0; i < num_bytes; i++)
				ra_snes_addrlist_add(address + i);
		}
		if (g_vb_state.cache_ready) {
			if (achievements_smart_cache_enabled() && g_vb_rtquery) {
				// Any byte not in the cached snapshot → resolve live via the
				// rtquery mailbox and add it as a dynamic address so it lands
				// in the batch snapshot from next frame on.
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
		if (g_vb_rtquery && achievements_rtquery_enabled() && !g_vb_state.collecting && num_bytes <= 4) {
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

static int vb_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
	if (!client || !game_loaded || !map || !g_vb_state.seladdr) return 0;

	rc_client_t *rc_client = (rc_client_t *)client;

	// ===================================================================
	// Smart Cache path (Tier 1): rtquery resolves list misses live, so the
	// address set adapts without periodic full re-collection.
	// ===================================================================
	if (achievements_smart_cache_enabled() && g_vb_rtquery) {

		if (ra_snes_addrlist_count() == 0 && !g_vb_state.cache_ready) {
			// Re-prime to WAITING before the all-zero collection frame so a
			// mid-game re-bootstrap (stall recovery) cannot fire on zeros.
			rc_client_reset(rc_client);
			g_vb_state.collecting = 1;
			ra_snes_addrlist_begin_collect();
			rc_client_do_frame(rc_client);
			g_vb_state.collecting = 0;
			int changed = ra_snes_addrlist_end_collect(map);
			if (changed)
				ra_log_write("VB SmartCache: Bootstrap done, %d addrs\n", ra_snes_addrlist_count());
			else
				ra_log_write("VB SmartCache: No addresses collected\n");
		} else if (!g_vb_state.cache_ready) {
			if (ra_snes_addrlist_is_ready(map)) {
				g_vb_state.cache_ready = 1;
				g_vb_state.last_resp_frame = 0;
				g_vb_state.game_frames = 0;
				g_vb_state.poll_logged = 0;
				clock_gettime(CLOCK_MONOTONIC, &g_vb_state.cache_time);
				// Discard the zero-primed bootstrap state (delta conditions
				// would otherwise see 0 -> real transitions and fire).
				rc_client_reset(rc_client);
				ra_log_write("VB SmartCache: Cache active! %d addrs (rc_client reset)\n", ra_snes_addrlist_count());
			}
		} else {
			uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
			seladdr_resync_if_backward(&g_vb_state, resp_frame, "VB");

			if (resp_frame > g_vb_state.last_resp_frame) {
				g_vb_state.last_resp_frame = resp_frame;
				g_vb_state.game_frames++;
				ra_frame_processed(resp_frame);

				// Skip frames whose VALCACHE ordering is unconfirmed (see helper).
				if (seladdr_frame_evaluable(map, "VB"))
					rc_client_do_frame(rc_client);

				// Dynamic-only prune (~1/min, only if dynamics piled up):
				// static bootstrap addresses stay; still-needed dynamics
				// re-add themselves via rtquery misses next frame.
				if (achievements_smart_cleanup_enabled()
						&& (g_vb_state.game_frames % 3000 == 0)
						&& ra_snes_addrlist_dyn_count() > 128) {
					int removed = ra_snes_addrlist_prune_dynamic(map);
					if (removed) {
						ra_log_write("VB SmartCache: pruned %d dynamic addrs (%d static kept)\n",
							removed, ra_snes_addrlist_count());
					}
				} else if (ra_snes_addrlist_has_pending()) {
					ra_snes_addrlist_flush_dynamic(map);
				}
			}
		}

		uint32_t milestone = g_vb_state.game_frames / 300;
		if (milestone > 0 && milestone != g_vb_state.poll_logged) {
			g_vb_state.poll_logged = milestone;
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double elapsed = (now.tv_sec - g_vb_state.cache_time.tv_sec)
				+ (now.tv_nsec - g_vb_state.cache_time.tv_nsec) / 1e9;
			double ms_per_cycle = (g_vb_state.game_frames > 0) ?
				(elapsed * 1000.0 / g_vb_state.game_frames) : 0.0;
			ra_log_write("POLL(VB-SC): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d\n",
				g_vb_state.last_resp_frame, g_vb_state.game_frames, elapsed, ms_per_cycle,
				ra_snes_addrlist_count());
		}
		return 1;
	}

	// ===================================================================
	// Legacy path (static addrlist, periodic re-collect)
	// ===================================================================

	if (ra_snes_addrlist_count() == 0 && !g_vb_state.cache_ready) {
		// Bootstrap: run one do_frame with zeros to discover needed addresses.
		// Re-prime to WAITING first so active triggers (mid-game re-bootstrap
		// after stall recovery) cannot fire on the all-zero reads.
		rc_client_reset(rc_client);
		g_vb_state.collecting = 1;
		ra_snes_addrlist_begin_collect();
		rc_client_do_frame(rc_client);
		g_vb_state.collecting = 0;
		int changed = ra_snes_addrlist_end_collect(map);
		if (changed) {
			ra_log_write("VB SelAddr: Bootstrap collection done, %d addrs written to DDRAM\n",
				ra_snes_addrlist_count());
		} else {
			ra_log_write("VB SelAddr: No addresses collected\n");
		}
	} else if (!g_vb_state.cache_ready) {
		// Wait for FPGA to respond with cached values
		if (ra_snes_addrlist_is_ready(map)) {
			g_vb_state.cache_ready = 1;
			g_vb_state.last_resp_frame = 0;
			g_vb_state.game_frames = 0;
			g_vb_state.poll_logged = 0;
			clock_gettime(CLOCK_MONOTONIC, &g_vb_state.cache_time);
			// Discard the zero-primed bootstrap state: delta conditions would
			// otherwise see 0 -> real transitions on the first genuine frame.
			rc_client_reset(rc_client);
			ra_log_write("VB SelAddr: Cache active! FPGA response matched request (rc_client reset).\n");
			// Dump address list on activation
			const uint32_t *a0 = ra_snes_addrlist_addrs();
			int ac = ra_snes_addrlist_count();
			int ad = ac < 20 ? ac : 20;
			char ah[256]; int ap = 0;
			for (int i = 0; i < ad && ap < (int)sizeof(ah) - 8; i++)
				ap += snprintf(ah + ap, sizeof(ah) - ap, "%05X ", a0[i]);
			ra_log_write("VB ADDRLIST[0..%d]: %s (total=%d)\n", ad - 1, ah, ac);
		}
	} else {
		// Normal frame processing from cache
		uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
		seladdr_resync_if_backward(&g_vb_state, resp_frame, "VB");
		if (resp_frame > g_vb_state.last_resp_frame) {
			g_vb_state.last_resp_frame = resp_frame;
			g_vb_state.game_frames++;
			ra_frame_processed(resp_frame);
			clock_gettime(CLOCK_MONOTONIC, &g_vb_state.stall_time);
			g_vb_state.stall_frame = resp_frame;

			// Skip achievement processing while a recollect revision is in
			// flight (newly collected addresses would read as 0).
			if (ra_snes_addrlist_is_ready(map)) {
				// Re-collect periodically to catch address changes
				int re_collect = !achievements_smart_cache_enabled()
					&& (g_vb_state.game_frames % 18000 == 0) && (g_vb_state.game_frames > 0);
				if (re_collect) {
					g_vb_state.collecting = 1;
					ra_snes_addrlist_begin_collect();
				}

				rc_client_do_frame(rc_client);

				if (re_collect) {
					g_vb_state.collecting = 0;
					if (ra_snes_addrlist_end_collect(map)) {
						ra_log_write("VB SelAddr: Address list refreshed, %d addrs\n",
							ra_snes_addrlist_count());
					}
				}
			}
		} else {
			seladdr_check_stall_recovery(&g_vb_state, resp_frame, "VB");
		}
	}

	uint32_t milestone = g_vb_state.game_frames / 300;
	if (milestone > 0 && milestone != g_vb_state.poll_logged) {
		g_vb_state.poll_logged = milestone;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = (now.tv_sec - g_vb_state.cache_time.tv_sec)
			+ (now.tv_nsec - g_vb_state.cache_time.tv_nsec) / 1e9;
		double ms_per_cycle = (g_vb_state.game_frames > 0) ?
			(elapsed * 1000.0 / g_vb_state.game_frames) : 0.0;
		ra_log_write("POLL(VB): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d\n",
			g_vb_state.last_resp_frame, g_vb_state.game_frames, elapsed, ms_per_cycle,
			ra_snes_addrlist_count());
	}

	return 1; // VB handled
#else
	return 0;
#endif
}

static int vb_calculate_hash(const char *rom_path, char *md5_hex_out)
{
	(void)rom_path;
	(void)md5_hex_out;
	return 0; // rcheevos hashes VB as whole-file MD5 — use default
}

static void vb_set_hardcore(int enabled)
{
	// bit 63: hardcore signal — the core forces cheats off (cheat table held
	// cleared), blocks savestate restore, and rejects TAS playback in RTL.
	user_io_status_set("[63]", enabled ? 1 : 0);
	ra_log_write("VB: Hardcore mode %s\n", enabled ? "enabled" : "disabled");
}

static int vb_detect_protocol(void *map)
{
	if (!ra_ramread_active(map)) {
		ra_log_write("VB: FPGA mirror not detected -- RA support unavailable\n");
		return 0;
	}
	// Virtual Boy always uses Selective Address
	g_vb_state.seladdr = 1;
	ra_log_write("VB FPGA protocol: Selective Address (selective address reading)\n");

	if (ra_rtquery_supported(map) && achievements_rtquery_enabled()) {
		g_vb_rtquery = 1;
		ra_rtquery_init(map);
		ra_log_write("VB: Realtime queries supported and ENABLED\n");
	} else if (ra_rtquery_supported(map)) {
		g_vb_rtquery = 0;
		ra_log_write("VB: Realtime queries supported but DISABLED by config\n");
	} else {
		g_vb_rtquery = 0;
		ra_log_write("VB: Realtime queries NOT supported (FPGA v1)\n");
	}
	return 1;
}

// ---------------------------------------------------------------------------
// Console handler definition
// ---------------------------------------------------------------------------

const console_handler_t g_console_virtualboy = {
	.init = vb_init,
	.reset = vb_reset,
	.read_memory = vb_read_memory,
	.poll = vb_poll,
	.calculate_hash = vb_calculate_hash,
	.set_hardcore = vb_set_hardcore,
	.detect_protocol = vb_detect_protocol,
	.console_id = 28,  // RC_CONSOLE_VIRTUAL_BOY
	.name = "VirtualBoy",
	.hardcore_protected = 1
};
