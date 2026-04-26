// achievements_nes.cpp — RetroAchievements NES-specific implementation

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
// NES Implementation
// ---------------------------------------------------------------------------

static void nes_init(void)
{
	// NES has no console-specific state
}

static void nes_reset(void)
{
	// NES has no console-specific state to reset
}

static uint32_t nes_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	// NES uses region-based layout (no Option C)
	return ra_ramread_nes_read(map, address, buffer, num_bytes);
}

static int nes_poll(void *map, void *client, int game_loaded)
{
	(void)map;
	(void)client;
	(void)game_loaded;
	// NES uses default frame tracking (no Option C)
	return 0; // fall through to achievements_poll default path
}

static int nes_detect_protocol(void *map)
{
	if (!ra_ramread_active(map)) {
		ra_log_write("NES: FPGA mirror not detected -- RA support unavailable\n");
		return 0;
	}
	const ra_header_t *hdr = (const ra_header_t *)map;
	ra_log_write("NES: FPGA mirror OK, regions=%u frame=%u\n",
		hdr->region_count, hdr->frame_counter);
	return 1;
}

static int nes_calculate_hash(const char *rom_path, char *md5_hex_out)
{
	// Fallback manual hashing
	FILE *f = fopen(rom_path, "rb");
	if (!f) {
		ra_log_write("NES: Failed to open ROM for hashing: %s\n", rom_path);
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

	// NES: skip iNES header ("NES\x1a") + optional 512-byte trainer
	if (file_size > 16 &&
		rom_data[0] == 0x4E && rom_data[1] == 0x45 &&  // 'N' 'E'
		rom_data[2] == 0x53 && rom_data[3] == 0x1A) {  // 'S' 0x1a
		uint32_t skip = 16;
		if (rom_data[6] & 0x04) skip += 512; // trainer present
		ra_log_write("NES: iNES header detected, skipping %u bytes (trainer=%d)\n",
			skip, (rom_data[6] & 0x04) ? 1 : 0);
		hash_data = rom_data + skip;
		hash_size = file_size - skip;
	}
	// FDS: skip fwNES FDS header ("FDS\x1a")
	else if (file_size > 16 &&
		rom_data[0] == 0x46 && rom_data[1] == 0x44 &&  // 'F' 'D'
		rom_data[2] == 0x53 && rom_data[3] == 0x1A) {  // 'S' 0x1a
		ra_log_write("NES: FDS header detected, skipping 16 bytes\n");
		hash_data = rom_data + 16;
		hash_size = file_size - 16;
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

static void nes_set_hardcore(int enabled)
{
	if (enabled) {
		user_io_status_set("[70]", 1);  // Disable cheats
		user_io_status_set("[20]", 1);  // Disable save states
		ra_log_write("NES: Hardcore mode enabled (cheats/states disabled)\n");
	} else {
		user_io_status_set("[70]", 0);
		user_io_status_set("[20]", 0);
		ra_log_write("NES: Hardcore mode disabled\n");
	}
}

// ---------------------------------------------------------------------------
// Console handler definition
// ---------------------------------------------------------------------------

const console_handler_t g_console_nes = {
	.init = nes_init,
	.reset = nes_reset,
	.read_memory = nes_read_memory,
	.poll = nes_poll,
	.calculate_hash = nes_calculate_hash,
	.set_hardcore = nes_set_hardcore,
	.detect_protocol = nes_detect_protocol,
	.console_id = 7,  // RC_CONSOLE_NINTENDO
	.name = "NES",
	.hardcore_protected = 1
};

const console_handler_t g_console_fds = {
	.init = nes_init,
	.reset = nes_reset,
	.read_memory = nes_read_memory,
	.poll = nes_poll,
	.calculate_hash = nes_calculate_hash,
	.set_hardcore = nes_set_hardcore,
	.detect_protocol = nes_detect_protocol,
	.console_id = 91,  // RC_CONSOLE_FAMICOM_DISK_SYSTEM
	.name = "Famicom Disk System",
	.hardcore_protected = 1
};
