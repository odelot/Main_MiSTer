// achievements_n64.cpp — RetroAchievements Nintendo 64-specific implementation

#include "achievements_console.h"
#include "achievements.h"
#include "ra_ramread.h"
#include "user_io.h"
#include "shmem.h"
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef HAS_RCHEEVOS
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#endif

// ---------------------------------------------------------------------------
// N64 State
// ---------------------------------------------------------------------------

static console_state_t g_n64_state = {};
static void    *g_n64_rdram_direct  = NULL; // direct RDRAM mmap for fallback reads
static uint8_t *g_n64_progress_buf  = NULL;
static size_t   g_n64_progress_size = 0;
static int      g_n64_resolve_cooldown = 0; // frames after resolution before re-enabling collection

// ---------------------------------------------------------------------------
// N64 Implementation
// ---------------------------------------------------------------------------

static void n64_init(void)
{
	memset(&g_n64_state, 0, sizeof(g_n64_state));
	g_n64_rdram_direct    = NULL;
	g_n64_progress_buf    = NULL;
	g_n64_progress_size   = 0;
	g_n64_resolve_cooldown = 0;

	// N64 savestates occupy 0x3C000000-0x3FFFFFFF (4 slots × 16MB),
	// colliding with the default RA base at 0x3D000000.
	// Move N64 RA mirror to the unused gap at 0x38000000.
	ra_ramread_set_base(0x38000000);
	ra_log_write("N64: Using RA DDRAM base 0x38000000 (avoiding SS slot collision)\n");
}

static void n64_reset(void)
{
	memset(&g_n64_state, 0, sizeof(g_n64_state));
	g_n64_resolve_cooldown = 0;
	if (g_n64_rdram_direct) {
		shmem_unmap(g_n64_rdram_direct, 0x800000);
		g_n64_rdram_direct = NULL;
	}
	if (g_n64_progress_buf) {
		free(g_n64_progress_buf);
		g_n64_progress_buf = NULL;
	}
	g_n64_progress_size = 0;
}

static uint32_t n64_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	if (g_n64_state.optionc) {
		// N64 rcheevos addresses are big-endian; RDRAM in DDR3 is
		// little-endian within 32-bit words.  XOR 3 on the low 2 bits converts.
		if (g_n64_state.collecting) {
			for (uint32_t i = 0; i < num_bytes; i++)
				ra_snes_addrlist_add((address + i) ^ 3);
		}
		// Always prefer direct RDRAM reads for consistency
		if (g_n64_rdram_direct) {
			const uint8_t *rdram = (const uint8_t *)g_n64_rdram_direct;
			for (uint32_t i = 0; i < num_bytes; i++) {
				uint32_t ddr_addr = (address + i) ^ 3;
				buffer[i] = (ddr_addr < 0x800000) ? rdram[ddr_addr] : 0;
			}
			return num_bytes;
		}
		if (g_n64_state.cache_ready) {
			for (uint32_t i = 0; i < num_bytes; i++)
				buffer[i] = ra_snes_addrlist_read_cached(map, (address + i) ^ 3);
			return num_bytes;
		}
		memset(buffer, 0, num_bytes);
		return num_bytes;
	}
	return 0;
}

static int n64_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
	if (!client || !game_loaded || !map || !g_n64_state.optionc) return 0;

	rc_client_t *rc_client = (rc_client_t *)client;

	// Ensure direct RDRAM mapping is available
	if (!g_n64_rdram_direct) {
		g_n64_rdram_direct = shmem_map(0x30000000, 0x800000);
		if (g_n64_rdram_direct)
			ra_log_write("N64 OptionC: Direct RDRAM mapped for fallback reads\n");
		else
			ra_log_write("N64 OptionC: WARNING - failed to map RDRAM for fallback!\n");
	}

	if (ra_snes_addrlist_count() == 0 && !g_n64_state.cache_ready) {
		// Phase 1: Bootstrap — collect addresses with zero values
		// Save rcheevos state before collection to prevent delta corruption
		size_t psize = rc_client_progress_size(rc_client);
		if (psize > g_n64_progress_size) {
			free(g_n64_progress_buf);
			g_n64_progress_buf = (uint8_t *)malloc(psize);
			g_n64_progress_size = psize;
		}
		if (g_n64_progress_buf)
			rc_client_serialize_progress_sized(rc_client, g_n64_progress_buf, g_n64_progress_size);

		g_n64_state.collecting = 1;
		ra_snes_addrlist_begin_collect();
		rc_client_do_frame(rc_client);
		g_n64_state.collecting = 0;

		// Restore state (undo delta/hit changes from zero-value reads)
		if (g_n64_progress_buf)
			rc_client_deserialize_progress_sized(rc_client, g_n64_progress_buf, g_n64_progress_size);

		int changed = ra_snes_addrlist_end_collect(map);
		if (changed) {
			g_n64_state.needs_recollect = 1;
			g_n64_state.resolve_pass = 0;
			ra_log_write("N64 OptionC: Bootstrap collection done, %d addrs written to DDRAM\n",
				ra_snes_addrlist_count());
		} else {
			ra_log_write("N64 OptionC: No addresses collected\n");
		}
	} else if (!g_n64_state.cache_ready) {
		// Phase 2/4: Wait for FPGA cache
		if (ra_snes_addrlist_is_ready(map)) {
			g_n64_state.cache_ready = 1;
			if (g_n64_state.needs_recollect) {
				ra_log_write("N64 OptionC: Cache active (pre-recollect). Will resolve pointers.\n");
			} else {
				if (g_n64_state.game_frames == 0) {
					// First activation after bootstrap
					g_n64_state.last_resp_frame = 0;
					g_n64_state.poll_logged = 0;
					clock_gettime(CLOCK_MONOTONIC, &g_n64_state.cache_time);
					ra_log_write("N64 OptionC: Cache active! FPGA response matched request.\n");
				} else {
					ra_log_write("N64 OptionC: Resolution complete, resuming (%d addrs)\n",
						ra_snes_addrlist_count());
				}
				if (g_n64_progress_buf)
					rc_client_deserialize_progress_sized(rc_client, g_n64_progress_buf, g_n64_progress_size);
				g_n64_resolve_cooldown = 30;
				ra_log_write("N64 OptionC: State restored via Phase 2, cooldown=30\n");
			}
		}
	} else if (g_n64_state.needs_recollect) {
		// Phase 3: Iterative pointer-resolution (multi-pass for AddAddress chains)
		g_n64_state.resolve_pass++;
		g_n64_state.collecting = 1;
		ra_snes_addrlist_begin_collect();
		rc_client_do_frame(rc_client);
		g_n64_state.collecting = 0;

		int changed = ra_snes_addrlist_end_collect(map);
		if (changed && g_n64_state.resolve_pass < 4) {
			ra_log_write("N64 OptionC: Pass %d found new addrs, %d total - re-resolving\n",
				g_n64_state.resolve_pass, ra_snes_addrlist_count());
			g_n64_state.cache_ready = 0; // wait for FPGA to respond with new addresses
		} else {
			ra_log_write("N64 OptionC: Pointer-resolution %s after %d passes, %d addrs\n",
				changed ? "done (max passes)" : "stable",
				g_n64_state.resolve_pass, ra_snes_addrlist_count());
			g_n64_state.needs_recollect = 0;
			if (changed) {
				g_n64_state.cache_ready = 0; // need final FPGA response (Phase 2 will deserialize)
			} else {
				// Stable: restore state directly since Phase 2 is skipped
				if (g_n64_progress_buf)
					rc_client_deserialize_progress_sized(rc_client, g_n64_progress_buf, g_n64_progress_size);
				g_n64_resolve_cooldown = 30;
				ra_log_write("N64 OptionC: State restored after stable resolution, cooldown=30\n");
			}
		}
	} else {
		// Phase 5: Normal frame processing
		uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
		if (resp_frame > g_n64_state.last_resp_frame) {
			g_n64_state.last_resp_frame = resp_frame;
			g_n64_state.game_frames++;
			ra_frame_processed(resp_frame);
			clock_gettime(CLOCK_MONOTONIC, &g_n64_state.stall_time);
			g_n64_state.stall_frame = resp_frame;

			if (g_n64_state.game_frames <= 5)
				ra_log_write("N64 OptionC: GameFrame %u (resp_frame=%u)\n",
					g_n64_state.game_frames, resp_frame);

			if (g_n64_resolve_cooldown > 0) {
				// Cooldown: evaluate normally without collection to avoid oscillation
				g_n64_resolve_cooldown--;
				rc_client_do_frame(rc_client);
			} else {
				// Per-frame collection: detect pointer changes
				size_t psize = rc_client_progress_size(rc_client);
				if (psize > g_n64_progress_size) {
					free(g_n64_progress_buf);
					g_n64_progress_buf = (uint8_t *)malloc(psize);
					g_n64_progress_size = psize;
				}
				if (g_n64_progress_buf)
					rc_client_serialize_progress_sized(rc_client, g_n64_progress_buf, g_n64_progress_size);

				g_n64_state.collecting = 1;
				ra_snes_addrlist_begin_collect();
				rc_client_do_frame(rc_client);
				g_n64_state.collecting = 0;

				if (ra_snes_addrlist_end_collect(map)) {
					// Pointer change detected: roll back and re-resolve
					if (g_n64_progress_buf)
						rc_client_deserialize_progress_sized(rc_client, g_n64_progress_buf, g_n64_progress_size);
					ra_log_write("N64 OptionC: Pointer change at game_frame=%u, %d addrs\n",
						g_n64_state.game_frames, ra_snes_addrlist_count());
					g_n64_state.cache_ready = 0;
					g_n64_state.needs_recollect = 1;
					g_n64_state.resolve_pass = 0;
				}
			}
		} else {
			optionc_check_stall_recovery(&g_n64_state, resp_frame, "N64");
		}
	}

	uint32_t milestone = g_n64_state.game_frames / 300;
	if (milestone > 0 && milestone != g_n64_state.poll_logged) {
		g_n64_state.poll_logged = milestone;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = (now.tv_sec - g_n64_state.cache_time.tv_sec)
			+ (now.tv_nsec - g_n64_state.cache_time.tv_nsec) / 1e9;
		double ms_per_cycle = (g_n64_state.game_frames > 0) ?
			(elapsed * 1000.0 / g_n64_state.game_frames) : 0.0;
		ra_log_write("POLL(N64): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d\n",
			g_n64_state.last_resp_frame, g_n64_state.game_frames, elapsed, ms_per_cycle,
			ra_snes_addrlist_count());
	}

	return 1; // N64 handled
#else
	return 0;
#endif
}

static int n64_calculate_hash(const char *rom_path, char *md5_hex_out)
{
#ifdef HAS_RCHEEVOS
	char abs_path[1024];
	if (rom_path[0] == '/') {
		snprintf(abs_path, sizeof(abs_path), "%s", rom_path);
	} else {
		extern const char *getRootDir(void);
		snprintf(abs_path, sizeof(abs_path), "%s/%s", getRootDir(), rom_path);
	}

	if (rc_hash_generate_from_file(md5_hex_out, 2, abs_path)) {
		ra_log_write("N64 hash: %s\n", md5_hex_out);
		return 1;
	}
	ra_log_write("N64: rc_hash_generate_from_file failed for %s\n", abs_path);
#endif
	return 0;
}

static void n64_set_hardcore(int enabled)
{
	ra_log_write("N64: Hardcore mode %s (no FPGA bits mapped)\n",
		enabled ? "enabled" : "disabled");
}

static void n64_detect_protocol(void *map)
{
	(void)map;
	// N64 always uses Option C
	g_n64_state.optionc = 1;
	ra_log_write("N64 FPGA protocol: Option C (selective address reading)\n");
}

// ---------------------------------------------------------------------------
// Console handler definition
// ---------------------------------------------------------------------------

const console_handler_t g_console_n64 = {
	.init = n64_init,
	.reset = n64_reset,
	.read_memory = n64_read_memory,
	.poll = n64_poll,
	.calculate_hash = n64_calculate_hash,
	.set_hardcore = n64_set_hardcore,
	.detect_protocol = n64_detect_protocol,
	.console_id = 2,  // RC_CONSOLE_NINTENDO_64
	.name = "N64"
};
