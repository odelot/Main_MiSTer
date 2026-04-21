// achievements_atari2600.cpp — RetroAchievements Atari 2600-specific implementation
//
// The Atari 2600 core lives inside the Atari7800_MiSTer. The FPGA copies
// the RIOT (M6532) internal 128-byte RAM to DDRAM each TIA VBlank,
// using the same "RACH" protocol as the NES mirror.
//
// Memory layout for rcheevos (RC_CONSOLE_ATARI_2600 = 25):
//   $0080-$00FF: RIOT internal RAM (128 bytes), region 0
//   The address range $0080-$00FF may appear mirrored at $0180-$01FF etc.

#include "achievements_console.h"
#include "achievements.h"
#include "ra_ramread.h"
#include "user_io.h"
#include "lib/md5/md5.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef HAS_RCHEEVOS
#include "rc_consoles.h"
#endif

// ---------------------------------------------------------------------------
// Atari 2600 Implementation
// ---------------------------------------------------------------------------

static uint32_t s_poll_count = 0;

static void atari2600_init(void)
{
	// No console-specific state
}

static void atari2600_reset(void)
{
	s_poll_count = 0;
}

static uint32_t atari2600_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	// region_count=1 -> 2600 (128-byte RIOT), region_count=2 -> 7800 (4KB RAM)
	uint8_t region_count = map ? ((const uint8_t *)map)[4] : 1;
	if (region_count == 2)
		return ra_ramread_atari7800_read(map, address, buffer, num_bytes);
	return ra_ramread_atari2600_read(map, address, buffer, num_bytes);
}

// Track last-seen values so we only log when something changes
static uint32_t s_last_frame   = 0xFFFFFFFF;
static int      s_last_nonzero = -1;

static int atari2600_poll(void *map, void *client, int game_loaded)
{
	(void)client;
	(void)game_loaded;

	s_poll_count++;

	// Log on first poll, then only when DDRAM state changes or every ~5 minutes
	int force_log = (s_poll_count == 1) || ((s_poll_count % 18000) == 0);

	const uint8_t *b = (const uint8_t *)map;
	if (!b) return 0;

	uint32_t frame_ctr = b[8]  | ((uint32_t)b[9]<<8)  | ((uint32_t)b[10]<<16) | ((uint32_t)b[11]<<24);

	int nonzero = 0;
	for (int i = 256; i < 384; i++) {
		if (b[i] != 0) nonzero++;
	}

	int changed = (frame_ctr != s_last_frame) || (nonzero != s_last_nonzero);

	if (force_log || changed) {
		uint32_t magic     = b[0]  | ((uint32_t)b[1]<<8)  | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24);
		uint16_t desc_size = b[20] | ((uint16_t)b[21]<<8);
		uint16_t desc_offs = b[22] | ((uint16_t)b[23]<<8);

		ra_log_write("A2600 poll=%u magic=0x%08X frame=%u desc_size=%u desc_offs=0x%04X RIOT=%d/128\n",
			s_poll_count, magic, frame_ctr, desc_size, desc_offs, nonzero);

		if (changed && nonzero == 0 && s_last_nonzero != 0) {
			// RIOT data just became all-zeros: dump raw bytes for diagnosis
			ra_log_write("A2600 DDRAM[0x00]: %02X%02X%02X%02X %02X%02X%02X%02X\n",
				b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
			ra_log_write("A2600 DDRAM[0x10]: %02X%02X%02X%02X %02X%02X%02X%02X\n",
				b[16], b[17], b[18], b[19], b[20], b[21], b[22], b[23]);
		}
		if (nonzero > 0 && s_last_nonzero == 0) {
			// RIOT data just became non-zero: dump first 16 bytes
			ra_log_write("A2600 RIOT[0x100..]: %02X %02X %02X %02X %02X %02X %02X %02X "
				"%02X %02X %02X %02X %02X %02X %02X %02X\n",
				b[256],b[257],b[258],b[259],b[260],b[261],b[262],b[263],
				b[264],b[265],b[266],b[267],b[268],b[269],b[270],b[271]);
		}

		s_last_frame   = frame_ctr;
		s_last_nonzero = nonzero;
	}

	// For 7800 mode: scan ram0 for diagnostic values
	if (b[4] == 2) {
		static uint8_t s_ram0_prev[512] = {0};
		// Scan first 512 bytes of ram0 (phys 0x2000-0x21FF)
		for (int j = 0; j < 512; j++) {
			uint8_t v = b[256 + j];
			if (v != s_ram0_prev[j]) {
				// Log changes to values 1,8,10,15 OR at specific key addrs
				if (v == 1 || v == 8 || v == 10 || v == 15 ||
				    j == 0x53 || j == 0x5B || j == 0xBC || j == 0xC4) {
					ra_log_write("A7800 RAM0[%03X]:%02X->%02X f=%u\n",
						j, (unsigned)s_ram0_prev[j], v, frame_ctr);
				}
				s_ram0_prev[j] = v;
			}
		}
	}

	return 0;
}

static void atari2600_detect_protocol(void *map)
{
	(void)map;
	// Region-based layout; no protocol detection needed
}

static int atari2600_calculate_hash(const char *rom_path, char *md5_hex_out)
{
	// Atari 2600: raw MD5 of the ROM file (no header to strip for plain .a26 ROMs)
	// .a78 files (Atari 7800 format) are not used for 2600 mode; we always get raw .a26
	FILE *f = fopen(rom_path, "rb");
	if (!f) {
		ra_log_write("ATARI2600: Failed to open ROM for hashing: %s\n", rom_path);
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

	MD5_CTX ctx;
	MD5Init(&ctx);
	MD5Update(&ctx, rom_data, file_size);
	unsigned char md5_bin[16];
	MD5Final(md5_bin, &ctx);

	for (int i = 0; i < 16; i++) {
		sprintf(&md5_hex_out[i * 2], "%02x", md5_bin[i]);
	}
	md5_hex_out[32] = '\0';

	free(rom_data);
	return 1;
}

static void atari2600_set_hardcore(int enabled)
{
	// Atari 2600 has no cheat or savestate status bits to control
	(void)enabled;
}

// ---------------------------------------------------------------------------
// Console handler definition
// ---------------------------------------------------------------------------

const console_handler_t g_console_atari2600 = {
	.init             = atari2600_init,
	.reset            = atari2600_reset,
	.read_memory      = atari2600_read_memory,
	.poll             = atari2600_poll,
	.calculate_hash   = atari2600_calculate_hash,
	.set_hardcore     = atari2600_set_hardcore,
	.detect_protocol  = atari2600_detect_protocol,
	.console_id       = 25,  // RC_CONSOLE_ATARI_2600
	.name             = "ATARI7800"  // matches user_io_get_core_name() for this core
};
