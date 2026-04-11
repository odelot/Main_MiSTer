#include <stdio.h>
#include <string.h>
#include <time.h>
#include "shmem.h"
#include "ra_ramread.h"

#define RA_DBG(fmt, ...) printf("\033[1;35mRA_MEM: " fmt "\033[0m\n", ##__VA_ARGS__)

void *ra_ramread_map(void)
{
	return shmem_map(RA_DDRAM_PHYS_BASE, RA_DDRAM_MAP_SIZE);
}

void ra_ramread_unmap(void *map)
{
	if (map) shmem_unmap(map, RA_DDRAM_MAP_SIZE);
}

int ra_ramread_active(const void *map)
{
	if (!map) return 0;
	const ra_header_t *hdr = (const ra_header_t *)map;
	return hdr->magic == RA_MAGIC;
}

uint32_t ra_ramread_frame(const void *map)
{
	if (!map) return 0;
	const ra_header_t *hdr = (const ra_header_t *)map;
	if (hdr->magic != RA_MAGIC) return 0;
	return hdr->frame_counter;
}

int ra_ramread_busy(const void *map)
{
	if (!map) return 0;
	const ra_header_t *hdr = (const ra_header_t *)map;
	return (hdr->flags & RA_FLAG_BUSY) ? 1 : 0;
}

static const ra_region_desc_t *get_region_desc(const void *map, int region_index)
{
	const ra_header_t *hdr = (const ra_header_t *)map;
	if (!map || hdr->magic != RA_MAGIC) return NULL;
	if (region_index < 0 || region_index >= hdr->region_count) return NULL;
	if (region_index >= RA_MAX_REGIONS) return NULL;

	// Descriptors start at offset 0x10, each is 8 bytes
	const uint8_t *base = (const uint8_t *)map;
	return (const ra_region_desc_t *)(base + 0x10 + region_index * 8);
}

const uint8_t *ra_ramread_region_data(const void *map, int region_index)
{
	const ra_region_desc_t *desc = get_region_desc(map, region_index);
	if (!desc || desc->size == 0) return NULL;

	const uint8_t *base = (const uint8_t *)map;
	return base + desc->ddram_offset;
}

uint16_t ra_ramread_region_size(const void *map, int region_index)
{
	const ra_region_desc_t *desc = get_region_desc(map, region_index);
	if (!desc) return 0;
	return desc->size;
}

uint8_t ra_ramread_nes_byte(const void *map, uint16_t nes_addr)
{
	// NES CPU address space:
	// $0000-$1FFF: Internal RAM (2KB, mirrored 4x) -> Region 0
	// $6000-$7FFF: Cart SRAM/WRAM -> Region 1
	if (nes_addr < 0x2000) {
		uint16_t offset = nes_addr & 0x07FF; // Resolve mirrors
		const uint8_t *data = ra_ramread_region_data(map, RA_NES_CPURAM_REGION);
		uint16_t size = ra_ramread_region_size(map, RA_NES_CPURAM_REGION);
		if (data && offset < size) return data[offset];
		return 0;
	}
	else if (nes_addr >= 0x6000 && nes_addr <= 0x7FFF) {
		uint16_t offset = nes_addr - 0x6000;
		const uint8_t *data = ra_ramread_region_data(map, RA_NES_CARTRAM_REGION);
		uint16_t size = ra_ramread_region_size(map, RA_NES_CARTRAM_REGION);
		if (data && offset < size) return data[offset];
		return 0;
	}

	return 0;
}

uint32_t ra_ramread_nes_read(const void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	for (uint32_t i = 0; i < num_bytes; i++) {
		buffer[i] = ra_ramread_nes_byte(map, (uint16_t)(address + i));
	}
	return num_bytes;
}

void ra_ramread_debug_dump(const void *map)
{
	RA_DBG("=== DDRAM Mirror Diagnostic Dump ===");
	RA_DBG("Base address: 0x%08X (size: 0x%X)", RA_DDRAM_PHYS_BASE, RA_DDRAM_MAP_SIZE);

	if (!map) {
		RA_DBG("ERROR: map pointer is NULL");
		return;
	}

	const ra_header_t *hdr = (const ra_header_t *)map;
	RA_DBG("Header raw bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
		((const uint8_t *)map)[0], ((const uint8_t *)map)[1],
		((const uint8_t *)map)[2], ((const uint8_t *)map)[3],
		((const uint8_t *)map)[4], ((const uint8_t *)map)[5],
		((const uint8_t *)map)[6], ((const uint8_t *)map)[7]);

	if (hdr->magic != RA_MAGIC) {
		RA_DBG("Magic: 0x%08X (INVALID - expected 0x%08X 'RACH')", hdr->magic, RA_MAGIC);
		RA_DBG("Mirror not active. FPGA core may not support RA or not started yet.");
		return;
	}

	RA_DBG("Magic: 0x%08X (OK - 'RACH')", hdr->magic);
	RA_DBG("Region count: %d", hdr->region_count);
	RA_DBG("Flags: 0x%02X (busy=%d)", hdr->flags, (hdr->flags & RA_FLAG_BUSY) ? 1 : 0);
	RA_DBG("Frame counter: %u", hdr->frame_counter);

	for (int i = 0; i < hdr->region_count && i < RA_MAX_REGIONS; i++) {
		const ra_region_desc_t *desc = (const ra_region_desc_t *)((const uint8_t *)map + 0x10 + i * 8);
		RA_DBG("Region %d: sdram_addr=0x%06X size=%u ddram_offset=0x%04X",
			i, desc->sdram_addr, desc->size, desc->ddram_offset);

		const uint8_t *data = (const uint8_t *)map + desc->ddram_offset;
		if (desc->size > 0 && desc->ddram_offset < RA_DDRAM_MAP_SIZE) {
			int dump_len = desc->size < 64 ? desc->size : 64;
			printf("\033[1;35mRA_MEM:   First %d bytes: ", dump_len);
			for (int j = 0; j < dump_len; j++) {
				printf("%02X ", data[j]);
				if ((j & 0xF) == 0xF && j + 1 < dump_len) printf("\n                         ");
			}
			printf("\033[0m\n");

			// Check if all zeros (common if mirror not writing yet)
			int all_zero = 1;
			for (int j = 0; j < dump_len; j++) {
				if (data[j] != 0) { all_zero = 0; break; }
			}
			if (all_zero) {
				RA_DBG("  WARNING: Region data is all zeros");
			}
		}
	}

	RA_DBG("=== End Diagnostic Dump ===");
}

void ra_ramread_debug_status(const void *map)
{
	if (!map) {
		RA_DBG("STATUS: not mapped");
		return;
	}
	const ra_header_t *hdr = (const ra_header_t *)map;
	if (hdr->magic != RA_MAGIC) {
		RA_DBG("STATUS: inactive (bad magic 0x%08X)", hdr->magic);
		return;
	}
	RA_DBG("STATUS: frame=%u regions=%d busy=%d",
		hdr->frame_counter, hdr->region_count, (hdr->flags & RA_FLAG_BUSY) ? 1 : 0);
}
