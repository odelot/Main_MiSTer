// achievements_snes.cpp — RetroAchievements SNES-specific implementation

#include "achievements_console.h"
#include "achievements.h"
#include "ra_ramread.h"
#include "user_io.h"
#include "lib/md5/md5.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef HAS_RCHEEVOS
#include "rc_client.h"
#include "rc_consoles.h"
#endif

// ---------------------------------------------------------------------------
// SNES State
// ---------------------------------------------------------------------------

static console_state_t g_snes_state = {0};

// ---------------------------------------------------------------------------
// SNES Option C Diagnostics
// ---------------------------------------------------------------------------

static void snes_optionc_dump_valcache(const char *label, void *map)
{
	if (!map) return;
	const uint8_t *base = (const uint8_t *)map;
	int addr_count = ra_snes_addrlist_count();
	const uint32_t *addrs = ra_snes_addrlist_addrs();

	// Response header
	const ra_val_resp_hdr_t *resp = (const ra_val_resp_hdr_t *)(base + RA_SNES_VALCACHE_OFFSET);
	ra_log_write("DUMP[%s] VALCACHE hdr: resp_id=%u resp_frame=%u\n",
		label, resp->response_id, resp->response_frame);

	// Raw VALCACHE bytes (first 32)
	const uint8_t *vals = base + RA_SNES_VALCACHE_OFFSET + 8;
	int dump_len = addr_count < 32 ? addr_count : 32;
	if (dump_len <= 0) dump_len = 32;
	char hex[200];
	int pos = 0;
	int non_zero = 0;
	for (int i = 0; i < dump_len && pos < (int)sizeof(hex) - 4; i++) {
		pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", vals[i]);
		if (vals[i]) non_zero++;
	}
	ra_log_write("DUMP[%s] VALCACHE raw[0..%d]: %s\n", label, dump_len - 1, hex);

	// Count non-zero in full address range
	int nz_all = 0;
	for (int i = 0; i < addr_count; i++)
		if (vals[i]) nz_all++;
	ra_log_write("DUMP[%s] non-zero: %d/%d\n", label, nz_all, addr_count);

	// Address+value pairs (first 10)
	int show = addr_count < 10 ? addr_count : 10;
	for (int i = 0; i < show; i++) {
		ra_log_write("DUMP[%s]   [%d] addr=0x%05X val=0x%02X\n", label, i, addrs[i], vals[i]);
	}
	
	// Also show a few known SMW addresses if present
	const uint32_t smw_addrs[] = {0x00019, 0x00DBF, 0x00F06, 0x00F31, 0x01490};
	const char *smw_names[] = {"playerSts", "dragonCoin", "lives", "coinCnt", "score"};
	for (int j = 0; j < 5; j++) {
		for (int i = 0; i < addr_count; i++) {
			if (addrs[i] == smw_addrs[j]) {
				ra_log_write("DUMP[%s]   SMW:%s(0x%05X)=0x%02X\n", 
					label, smw_names[j], smw_addrs[j], vals[i]);
				break;
			}
		}
	}

	// ADDRLIST header readback
	const ra_addr_req_hdr_t *ahdr = (const ra_addr_req_hdr_t *)(base + RA_SNES_ADDRLIST_OFFSET);
	ra_log_write("DUMP[%s] ADDRLIST hdr readback: addr_count=%u request_id=%u\n",
		label, ahdr->addr_count, ahdr->request_id);

	// ADDRLIST address readback (first 5 from DDRAM)
	const uint32_t *ddram_addrs = (const uint32_t *)(base + RA_SNES_ADDRLIST_OFFSET + 8);
	int rshow = addr_count < 5 ? addr_count : 5;
	for (int i = 0; i < rshow; i++) {
		ra_log_write("DUMP[%s]   DDRAM_ADDR[%d]=0x%08X (local=0x%05X)\n", 
			label, i, ddram_addrs[i], addrs[i]);
	}

	// FPGA debug words
	const uint8_t *dbg8 = (const uint8_t *)(base + 0x10);
	uint16_t ok_cnt      = dbg8[0] | (dbg8[1] << 8);
	uint16_t timeout_cnt = dbg8[2] | (dbg8[3] << 8);
	uint16_t first_dout  = dbg8[4] | (dbg8[5] << 8);
	uint8_t  dispatch_cnt = dbg8[6];
	uint8_t  fpga_ver     = dbg8[7];

	const uint8_t *dbg8b = (const uint8_t *)(base + 0x18);
	uint16_t max_timeout  = dbg8b[0] | (dbg8b[1] << 8);
	uint16_t bsram_cnt    = dbg8b[2] | (dbg8b[3] << 8);
	uint16_t wram_cnt     = dbg8b[4] | (dbg8b[5] << 8);
	uint16_t first_addr   = dbg8b[6] | (dbg8b[7] << 8);

	ra_log_write("DUMP[%s] FPGA_DBG: ver=0x%02X ok=%u timeout=%u first_dout=0x%04X dispatch=%u\n",
		label, fpga_ver, ok_cnt, timeout_cnt, first_dout, dispatch_cnt);
	ra_log_write("DUMP[%s] FPGA_DBG2: wram=%u bsram=%u first_addr=0x%04X max_timeout=%u\n",
		label, wram_cnt, bsram_cnt, first_addr, max_timeout);

	// Bypass check: addresses should match pattern: addr[7:0] ^ 0xA5
	int bypass_fail = 0;
	for (int i = 0; i < addr_count && i < 16; i++) {
		uint8_t expected = (addrs[i] & 0xFF) ^ 0xA5;
		if (vals[i] != expected) bypass_fail++;
	}
	if (bypass_fail > 0) {
		ra_log_write("DUMP[%s] Bypass check: %d/%d mismatches (expected addr[7:0]^0xA5)\n",
			label, bypass_fail, addr_count < 16 ? addr_count : 16);
	}
}

// ---------------------------------------------------------------------------
// SNES Implementation
// ---------------------------------------------------------------------------

static void snes_init(void)
{
	memset(&g_snes_state, 0, sizeof(g_snes_state));
	ra_snes_addrlist_init();
}

static void snes_reset(void)
{
	g_snes_state.optionc = 0;
	g_snes_state.collecting = 0;
	g_snes_state.cache_ready = 0;
	g_snes_state.last_resp_frame = 0;
	g_snes_state.game_frames = 0;
	g_snes_state.poll_logged = 0;
}

static uint32_t snes_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	if (g_snes_state.optionc) {
		// Option C: selective address reading
		if (g_snes_state.collecting) {
			for (uint32_t i = 0; i < num_bytes; i++)
				ra_snes_addrlist_add(address + i);
		}
		if (g_snes_state.cache_ready) {
			for (uint32_t i = 0; i < num_bytes; i++)
				buffer[i] = ra_snes_addrlist_read_cached(map, address + i);
			return num_bytes;
		}
		memset(buffer, 0, num_bytes);
		return num_bytes;
	} else {
		// VBlank-gated: read from full WRAM mirror
		return ra_ramread_snes_read(map, address, buffer, num_bytes);
	}
}

static void snes_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
	if (!client || !game_loaded || !map) return;
	
	rc_client_t *rc_client = (rc_client_t *)client;

	if (g_snes_state.optionc) {
		// Option C selective reading mode
		if (ra_snes_addrlist_count() == 0 && !g_snes_state.cache_ready) {
			// Bootstrap: run one do_frame with zeros to discover needed addresses
			g_snes_state.collecting = 1;
			ra_snes_addrlist_begin_collect();
			rc_client_do_frame(rc_client);
			g_snes_state.collecting = 0;
			int changed = ra_snes_addrlist_end_collect(map);
			if (changed) {
				ra_log_write("SNES OptionC: Bootstrap collection done, %d addrs written to DDRAM\n",
					ra_snes_addrlist_count());
				snes_optionc_dump_valcache("bootstrap", map);
			} else {
				ra_log_write("SNES OptionC: No addresses collected — achievements may have no memory refs\n");
			}
		} else if (!g_snes_state.cache_ready) {
			// Wait for FPGA to respond with cached values
			if (ra_snes_addrlist_is_ready(map)) {
				g_snes_state.cache_ready = 1;
				g_snes_state.last_resp_frame = 0;
				g_snes_state.game_frames = 0;
				g_snes_state.poll_logged = 0;
				clock_gettime(CLOCK_MONOTONIC, &g_snes_state.cache_time);
				ra_log_write("SNES OptionC: Cache active! FPGA response matched request.\n");
				snes_optionc_dump_valcache("cache-active", map);
			}
		} else {
			// Normal frame processing from cache
			uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
			if (resp_frame > g_snes_state.last_resp_frame) {
				g_snes_state.last_resp_frame = resp_frame;
				g_snes_state.game_frames++;

				// Dump first 5 frames after cache became active
				if (g_snes_state.game_frames <= 5) {
					ra_log_write("SNES OptionC: GameFrame %u (resp_frame=%u)\n",
						g_snes_state.game_frames, resp_frame);
					snes_optionc_dump_valcache("early-frame", map);
				}

				// Periodically re-collect to catch address changes (every ~5 min)
				int re_collect = (g_snes_state.game_frames % 18000 == 0) && (g_snes_state.game_frames > 0);
				if (re_collect) {
					g_snes_state.collecting = 1;
					ra_snes_addrlist_begin_collect();
				}

				rc_client_do_frame(rc_client);

				if (re_collect) {
					g_snes_state.collecting = 0;
					if (ra_snes_addrlist_end_collect(map)) {
						ra_log_write("SNES OptionC: Address list refreshed, %d addrs\n",
							ra_snes_addrlist_count());
					}
				}
			}
		}

		// Periodic SNES debug — log once per 300-frame milestone
		uint32_t milestone = g_snes_state.game_frames / 300;
		if (milestone > 0 && milestone != g_snes_state.poll_logged) {
			g_snes_state.poll_logged = milestone;
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double elapsed = (now.tv_sec - g_snes_state.cache_time.tv_sec)
				+ (now.tv_nsec - g_snes_state.cache_time.tv_nsec) / 1e9;
			double ms_per_cycle = (g_snes_state.game_frames > 0) ?
				(elapsed * 1000.0 / g_snes_state.game_frames) : 0.0;
			ra_log_write("POLL(SNES): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d\n",
				g_snes_state.last_resp_frame, g_snes_state.game_frames, elapsed, ms_per_cycle,
				ra_snes_addrlist_count());
			
			// Dump VALCACHE every 1800 game frames (~30s)
			if ((g_snes_state.game_frames % 1800) == 0) {
				snes_optionc_dump_valcache("periodic", map);
			}
		}
	}
	// VBlank-gated mode uses default frame tracking in main achievements_poll()
#endif
}

static int snes_calculate_hash(const char *rom_path, char *md5_hex_out)
{
	// SNES: skip optional 512-byte SMC/SWC copier header
	FILE *f = fopen(rom_path, "rb");
	if (!f) {
		ra_log_write("SNES: Failed to open ROM for hashing: %s\n", rom_path);
		return 0;
	}

	fseek(f, 0, SEEK_END);
	long file_size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (file_size <= 0) {
		fclose(f);
		return 0;
	}

	uint8_t *rom_data = (uint8_t *)malloc(file_size);
	if (!rom_data) {
		fclose(f);
		return 0;
	}

	size_t nread = fread(rom_data, 1, file_size, f);
	fclose(f);
	
	if ((long)nread != file_size) {
		free(rom_data);
		return 0;
	}

	const uint8_t *hash_data = rom_data;
	long hash_size = file_size;

	// SNES: skip optional 512-byte SMC/SWC copier header
	if ((file_size % 1024) == 512 && file_size > 512) {
		ra_log_write("SNES: SMC header detected (file_size %% 1024 == 512), skipping 512 bytes\n");
		hash_data = rom_data + 512;
		hash_size = file_size - 512;
	}

	// Calculate MD5
	MD5_CTX ctx;
	MD5Init(&ctx);
	MD5Update(&ctx, hash_data, hash_size);
	unsigned char md5_bin[16];
	MD5Final(md5_bin, &ctx);

	for (int i = 0; i < 16; i++) {
		sprintf(&md5_hex_out[i * 2], "%02x", md5_bin[i]);
	}
	md5_hex_out[32] = '\0';

	free(rom_data);
	return 1;
}

static void snes_set_hardcore(int enabled)
{
	if (enabled) {
		user_io_status_set("[58]", 1);  // Disable cheats
		user_io_status_set("[24]", 1);  // Disable save states
		ra_log_write("SNES: Hardcore mode enabled (cheats/states disabled)\n");
	} else {
		user_io_status_set("[58]", 0);
		user_io_status_set("[24]", 0);
		ra_log_write("SNES: Hardcore mode disabled\n");
	}
}

// Called from main achievements code to detect protocol type
void snes_detect_protocol(void *map)
{
	if (!map) return;
	
	const ra_header_t *hdr = (const ra_header_t *)map;
	if (hdr->region_count == 0) {
		g_snes_state.optionc = 1;
		ra_log_write("SNES FPGA protocol: Option C (selective address reading)\n");
	} else {
		g_snes_state.optionc = 0;
		ra_log_write("SNES FPGA protocol: VBlank-gated full mirror (region_count=%d)\n", 
			hdr->region_count);
	}
}

// ---------------------------------------------------------------------------
// Console handler definition
// ---------------------------------------------------------------------------

const console_handler_t g_console_snes = {
	.init = snes_init,
	.reset = snes_reset,
	.read_memory = snes_read_memory,
	.poll = snes_poll,
	.calculate_hash = snes_calculate_hash,
	.set_hardcore = snes_set_hardcore,
	.console_id = 3,  // RC_CONSOLE_SUPER_NINTENDO
	.name = "SNES"
};
