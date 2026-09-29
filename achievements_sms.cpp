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
static int g_sms_rtquery = 0;

// ---------------------------------------------------------------------------
// SMS Implementation
// ---------------------------------------------------------------------------

static void sms_init(void)
{
	memset(&g_sms_state, 0, sizeof(g_sms_state));
	g_sms_rtquery = 0;
}

static void sms_reset(void)
{
	memset(&g_sms_state, 0, sizeof(g_sms_state));
	g_sms_rtquery = 0;
}

static uint32_t sms_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	if (g_sms_state.seladdr) {
		if (g_sms_state.collecting) {
			for (uint32_t i = 0; i < num_bytes; i++)
				ra_snes_addrlist_add(address + i);
		}
		if (g_sms_state.cache_ready) {
			if (achievements_smart_cache_enabled() && g_sms_rtquery) {
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
		if (g_sms_rtquery && achievements_rtquery_enabled() && !g_sms_state.collecting && num_bytes <= 4) {
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

static int sms_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
	if (!client || !game_loaded || !map || !g_sms_state.seladdr) return 0;

	rc_client_t *rc_client = (rc_client_t *)client;

	// ===================================================================
	// Smart Cache path (Tier 1): rtquery resolves list misses live, so the
	// address set adapts without periodic full re-collection.
	// ===================================================================
	if (achievements_smart_cache_enabled() && g_sms_rtquery) {

		if (ra_snes_addrlist_count() == 0 && !g_sms_state.cache_ready) {
			// Re-prime to WAITING before the all-zero collection frame so a
			// mid-game re-bootstrap (stall recovery) cannot fire on zeros.
			rc_client_reset(rc_client);
			g_sms_state.collecting = 1;
			ra_snes_addrlist_begin_collect();
			rc_client_do_frame(rc_client);
			g_sms_state.collecting = 0;
			int changed = ra_snes_addrlist_end_collect(map);
			if (changed)
				ra_log_write("SMS SmartCache: Bootstrap done, %d addrs\n", ra_snes_addrlist_count());
			else
				ra_log_write("SMS SmartCache: No addresses collected\n");
		} else if (!g_sms_state.cache_ready) {
			if (ra_snes_addrlist_is_ready(map)) {
				g_sms_state.cache_ready = 1;
				g_sms_state.last_resp_frame = 0;
				g_sms_state.game_frames = 0;
				g_sms_state.poll_logged = 0;
				clock_gettime(CLOCK_MONOTONIC, &g_sms_state.cache_time);
				// Discard the zero-primed bootstrap state (delta conditions
				// would otherwise see 0 -> real transitions and fire).
				rc_client_reset(rc_client);
				ra_log_write("SMS SmartCache: Cache active! %d addrs (rc_client reset)\n", ra_snes_addrlist_count());
			}
		} else {
			uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
			seladdr_resync_if_backward(&g_sms_state, resp_frame, "SMS");

			if (resp_frame > g_sms_state.last_resp_frame) {
				g_sms_state.last_resp_frame = resp_frame;
				g_sms_state.game_frames++;
				ra_frame_processed(resp_frame);

				// Skip frames whose VALCACHE ordering is unconfirmed (see helper).
				if (seladdr_frame_evaluable(map, "SMS"))
					rc_client_do_frame(rc_client);

				// Dynamic-only prune (~1/min, only if dynamics piled up):
				// static bootstrap addresses stay; still-needed dynamics
				// re-add themselves via rtquery misses next frame.
				if (achievements_smart_cleanup_enabled()
						&& (g_sms_state.game_frames % 3600 == 0)
						&& ra_snes_addrlist_dyn_count() > 128) {
					int removed = ra_snes_addrlist_prune_dynamic(map);
					if (removed) {
						ra_log_write("SMS SmartCache: pruned %d dynamic addrs (%d static kept)\n",
							removed, ra_snes_addrlist_count());
					}
				} else if (ra_snes_addrlist_has_pending()) {
					ra_snes_addrlist_flush_dynamic(map);
				}
			}
		}

		uint32_t milestone = g_sms_state.game_frames / 300;
		if (milestone > 0 && milestone != g_sms_state.poll_logged) {
			g_sms_state.poll_logged = milestone;
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double elapsed = (now.tv_sec - g_sms_state.cache_time.tv_sec)
				+ (now.tv_nsec - g_sms_state.cache_time.tv_nsec) / 1e9;
			double ms_per_cycle = (g_sms_state.game_frames > 0) ?
				(elapsed * 1000.0 / g_sms_state.game_frames) : 0.0;
			ra_log_write("POLL(SMS-SC): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d\n",
				g_sms_state.last_resp_frame, g_sms_state.game_frames, elapsed, ms_per_cycle,
				ra_snes_addrlist_count());
		}
		return 1;
	}

	// ===================================================================
	// Legacy path (static addrlist, periodic re-collect)
	// ===================================================================

	if (ra_snes_addrlist_count() == 0 && !g_sms_state.cache_ready) {
		// Bootstrap: run one do_frame with zeros to discover needed addresses.
		// Re-prime to WAITING first so active triggers (mid-game re-bootstrap
		// after stall recovery) cannot fire on the all-zero reads.
		rc_client_reset(rc_client);
		g_sms_state.collecting = 1;
		ra_snes_addrlist_begin_collect();
		rc_client_do_frame(rc_client);
		g_sms_state.collecting = 0;
		int changed = ra_snes_addrlist_end_collect(map);
		if (changed) {
			ra_log_write("SMS SelAddr: Bootstrap collection done, %d addrs written to DDRAM\n",
				ra_snes_addrlist_count());
		} else {
			ra_log_write("SMS SelAddr: No addresses collected\n");
		}
	} else if (!g_sms_state.cache_ready) {
		// Wait for FPGA to respond with cached values
		if (ra_snes_addrlist_is_ready(map)) {
			g_sms_state.cache_ready = 1;
			g_sms_state.last_resp_frame = 0;
			g_sms_state.game_frames = 0;
			g_sms_state.poll_logged = 0;
			clock_gettime(CLOCK_MONOTONIC, &g_sms_state.cache_time);
			// Discard the zero-primed bootstrap state: delta conditions would
			// otherwise see 0 -> real transitions on the first genuine frame.
			rc_client_reset(rc_client);
			ra_log_write("SMS SelAddr: Cache active! FPGA response matched request (rc_client reset).\n");
			// Dump address list on activation
			const uint32_t *a0 = ra_snes_addrlist_addrs();
			int ac = ra_snes_addrlist_count();
			int ad = ac < 20 ? ac : 20;
			char ah[256]; int ap = 0;
			for (int i = 0; i < ad && ap < (int)sizeof(ah) - 8; i++)
				ap += snprintf(ah + ap, sizeof(ah) - ap, "%04X ", a0[i]);
			ra_log_write("SMS ADDRLIST[0..%d]: %s (total=%d)\n", ad - 1, ah, ac);
		}
	} else {
		// Normal frame processing from cache
		uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
		seladdr_resync_if_backward(&g_sms_state, resp_frame, "SMS");
		if (resp_frame > g_sms_state.last_resp_frame) {
			g_sms_state.last_resp_frame = resp_frame;
			g_sms_state.game_frames++;
			ra_frame_processed(resp_frame);
			clock_gettime(CLOCK_MONOTONIC, &g_sms_state.stall_time);
			g_sms_state.stall_frame = resp_frame;

			// Skip achievement processing while a recollect revision is in
			// flight (newly collected addresses would read as 0).
			if (ra_snes_addrlist_is_ready(map)) {
				// Re-collect every ~5 min to catch address changes
				// Smart cache mode: skip re-collect (no dynamic pointers in SMS)
				int re_collect = !achievements_smart_cache_enabled()
					&& (g_sms_state.game_frames % 18000 == 0) && (g_sms_state.game_frames > 0);
				if (re_collect) {
					g_sms_state.collecting = 1;
					ra_snes_addrlist_begin_collect();
				}

				rc_client_do_frame(rc_client);

				if (re_collect) {
					g_sms_state.collecting = 0;
					if (ra_snes_addrlist_end_collect(map)) {
						ra_log_write("SMS SelAddr: Address list refreshed, %d addrs\n",
							ra_snes_addrlist_count());
					}
				}
			}
		} else {
			seladdr_check_stall_recovery(&g_sms_state, resp_frame, "SMS");
		}
	}

	uint32_t milestone = g_sms_state.game_frames / 300;
	if (milestone > 0 && milestone != g_sms_state.poll_logged) {
		g_sms_state.poll_logged = milestone;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = (now.tv_sec - g_sms_state.cache_time.tv_sec)
			+ (now.tv_nsec - g_sms_state.cache_time.tv_nsec) / 1e9;
		double ms_per_cycle = (g_sms_state.game_frames > 0) ?
			(elapsed * 1000.0 / g_sms_state.game_frames) : 0.0;
		ra_log_write("POLL(SMS): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d\n",
			g_sms_state.last_resp_frame, g_sms_state.game_frames, elapsed, ms_per_cycle,
			ra_snes_addrlist_count());
	}

	return 1; // SMS handled
#else
	return 0;
#endif
}

static int sms_calculate_hash(const char *rom_path, char *md5_hex_out)
{
	(void)rom_path;
	(void)md5_hex_out;
	return 0; // use default MD5
}

static void sms_set_hardcore(int enabled)
{
	// bit 63: hardcore signal (moved from 55 — upstream SMS 20260603+ uses
	// status[56:55] for USERIO SNAC/Gear2Gear)
	user_io_status_set("[63]", enabled ? 1 : 0);
	user_io_status_set("[24]", enabled ? 1 : 0); // disable cheats OSD toggle
	ra_log_write("SMS: Hardcore mode %s\n", enabled ? "enabled" : "disabled");
}

static int sms_detect_protocol(void *map)
{
	if (!ra_ramread_active(map)) {
		ra_log_write("SMS: FPGA mirror not detected -- RA support unavailable\n");
		return 0;
	}
	// SMS always uses Selective Address
	g_sms_state.seladdr = 1;
	ra_log_write("SMS FPGA protocol: Selective Address (selective address reading)\n");

	if (ra_rtquery_supported(map) && achievements_rtquery_enabled()) {
		g_sms_rtquery = 1;
		ra_rtquery_init(map);
		ra_log_write("SMS: Realtime queries supported and ENABLED\n");
		// Cap the rtquery busy-wait when the lightgun A/B test is active so a
		// mailbox stall can never delay mouse->SPI input forwarding.
		if (achievements_justifier_test()) {
			ra_rtquery_set_spin_limit(2000);
			ra_log_write("SMS: JUSTIFIER TEST active -- rtquery spin capped\n");
		}
	} else if (ra_rtquery_supported(map)) {
		g_sms_rtquery = 0;
		ra_log_write("SMS: Realtime queries supported but DISABLED by config\n");
	} else {
		g_sms_rtquery = 0;
		ra_log_write("SMS: Realtime queries NOT supported (FPGA v1)\n");
	}
	return 1;
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
	.detect_protocol = sms_detect_protocol,
	.console_id = 11,  // RC_CONSOLE_MASTER_SYSTEM (also handles Game Gear ID 15)
	.name = "SMS",
	.hardcore_protected = 1
};
