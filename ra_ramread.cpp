#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "shmem.h"
#include "ra_ramread.h"

#define RA_DBG(fmt, ...) printf("\033[1;35mRA_MEM: " fmt "\033[0m\n", ##__VA_ARGS__)

static uint32_t g_ra_ddram_base = RA_DDRAM_PHYS_BASE;

void ra_ramread_set_base(uint32_t phys_base)
{
	g_ra_ddram_base = phys_base;
}

uint32_t ra_ramread_get_base(void)
{
	return g_ra_ddram_base;
}

void *ra_ramread_map(void)
{
	return shmem_map(g_ra_ddram_base, RA_DDRAM_MAP_SIZE);
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

uint32_t ra_ramread_snes_read(const void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	if (!map) { memset(buffer, 0, num_bytes); return num_bytes; }
	const uint8_t *base = (const uint8_t *)map;
	const ra_header_t *hdr = (const ra_header_t *)base;
	if (hdr->magic != RA_MAGIC) { memset(buffer, 0, num_bytes); return num_bytes; }

	// BSRAM size stored at offset 0x0C (reserved2 field)
	uint32_t bsram_sz = hdr->reserved2;

	for (uint32_t i = 0; i < num_bytes; i++) {
		uint32_t addr = address + i;
		if (addr < RA_SNES_WRAM_SIZE) {
			// WRAM: 128KB at mirror offset 0x100
			uint32_t off = RA_SNES_WRAM_OFFSET + addr;
			if (off < RA_DDRAM_MAP_SIZE)
				buffer[i] = base[off];
			else
				buffer[i] = 0;
		} else {
			// BSRAM: at mirror offset 0x20100
			uint32_t sram_off = addr - RA_SNES_WRAM_SIZE;
			if (sram_off < bsram_sz) {
				uint32_t off = RA_SNES_BSRAM_OFFSET + sram_off;
				if (off < RA_DDRAM_MAP_SIZE)
					buffer[i] = base[off];
				else
					buffer[i] = 0;
			} else {
				buffer[i] = 0;
			}
		}
	}
	return num_bytes;
}

void ra_ramread_debug_dump(const void *map)
{
	RA_DBG("=== DDRAM Mirror Diagnostic Dump ===");
	RA_DBG("Base address: 0x%08X (size: 0x%X)", g_ra_ddram_base, RA_DDRAM_MAP_SIZE);

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
		if (desc->size > 0 && (uint32_t)desc->ddram_offset < RA_DDRAM_MAP_SIZE) {
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

// ======================================================================
// Option C: Selective Address Reading (SNES)
// ======================================================================

static int s_addr_cmp(const void *a, const void *b)
{
	uint32_t va = *(const uint32_t *)a;
	uint32_t vb = *(const uint32_t *)b;
	return (va > vb) - (va < vb);
}

static uint32_t s_snes_addrs[RA_SNES_MAX_ADDRS];
static int      s_snes_addr_count = 0;
static uint32_t s_snes_request_id = 0;
static int      s_snes_collecting = 0;

#define COLLECT_BUF_MAX (RA_SNES_MAX_ADDRS * 4)
static uint32_t s_collect_buf[COLLECT_BUF_MAX];
static int      s_collect_count = 0;

void ra_snes_addrlist_init(void)
{
	s_snes_addr_count = 0;
	s_snes_request_id = 0;
	s_snes_collecting = 0;
	s_collect_count = 0;
}

void ra_snes_addrlist_begin_collect(void)
{
	s_snes_collecting = 1;
	s_collect_count = 0;
}

void ra_snes_addrlist_add(uint32_t addr)
{
	if (!s_snes_collecting) return;
	if (s_collect_count < COLLECT_BUF_MAX)
		s_collect_buf[s_collect_count++] = addr;
}

int ra_snes_addrlist_end_collect(void *map)
{
	s_snes_collecting = 0;
	if (s_collect_count == 0) return 0;

	// Sort
	qsort(s_collect_buf, s_collect_count, sizeof(uint32_t), s_addr_cmp);

	// Deduplicate
	int new_count = 0;
	for (int i = 0; i < s_collect_count; i++) {
		if (new_count == 0 || s_collect_buf[i] != s_collect_buf[new_count - 1])
			s_collect_buf[new_count++] = s_collect_buf[i];
	}
	if (new_count > RA_SNES_MAX_ADDRS)
		new_count = RA_SNES_MAX_ADDRS;

	// Compare with current list
	int changed = (new_count != s_snes_addr_count);
	if (!changed) {
		for (int i = 0; i < new_count; i++) {
			if (s_collect_buf[i] != s_snes_addrs[i]) { changed = 1; break; }
		}
	}
	if (!changed) return 0;

	// Update local list
	memcpy(s_snes_addrs, s_collect_buf, new_count * sizeof(uint32_t));
	s_snes_addr_count = new_count;
	s_snes_request_id++;

	// Write to DDRAM
	if (!map) return 1;
	uint8_t *base = (uint8_t *)map;

	// Write addresses first (before header, so FPGA sees consistent data)
	uint32_t *addrs = (uint32_t *)(base + RA_SNES_ADDRLIST_OFFSET + 8);
	memcpy(addrs, s_snes_addrs, new_count * sizeof(uint32_t));
	__sync_synchronize();

	// Write header: addr_count first, then request_id as the "commit" signal.
	// FPGA reads both atomically as a 64-bit word. If it catches an in-between
	// state, seeing old request_id with new addr_count is safe (it will process
	// addresses but ARM won't see the response as ready until request_id matches).
	ra_addr_req_hdr_t *hdr = (ra_addr_req_hdr_t *)(base + RA_SNES_ADDRLIST_OFFSET);
	hdr->addr_count = new_count;
	__sync_synchronize();
	hdr->request_id = s_snes_request_id;
	__sync_synchronize();

	RA_DBG("AddrList: %d addrs, request_id=%u, first_addr=0x%05X",
		new_count, s_snes_request_id,
		new_count > 0 ? s_snes_addrs[0] : 0);
	return 1;
}

uint8_t ra_snes_addrlist_read_cached(const void *map, uint32_t addr)
{
	if (!map || s_snes_addr_count == 0) return 0;

	// Binary search
	int lo = 0, hi = s_snes_addr_count - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (s_snes_addrs[mid] == addr) {
			const uint8_t *vals = (const uint8_t *)map + RA_SNES_VALCACHE_OFFSET + 8;
			return vals[mid];
		}
		if (s_snes_addrs[mid] < addr) lo = mid + 1;
		else hi = mid - 1;
	}
	return 0;
}

int ra_snes_addrlist_is_ready(const void *map)
{
	if (!map || s_snes_addr_count == 0 || s_snes_request_id == 0) return 0;
	const uint8_t *base = (const uint8_t *)map;
	const ra_val_resp_hdr_t *resp = (const ra_val_resp_hdr_t *)(base + RA_SNES_VALCACHE_OFFSET);
	return resp->response_id == s_snes_request_id;
}

int ra_snes_addrlist_count(void)
{
	return s_snes_addr_count;
}

const uint32_t *ra_snes_addrlist_addrs(void)
{
	return s_snes_addrs;
}

uint32_t ra_snes_addrlist_request_id(void)
{
	return s_snes_request_id;
}

uint32_t ra_snes_addrlist_response_frame(const void *map)
{
	if (!map) return 0;
	const uint8_t *base = (const uint8_t *)map;
	const ra_val_resp_hdr_t *resp = (const ra_val_resp_hdr_t *)(base + RA_SNES_VALCACHE_OFFSET);
	return resp->response_frame;
}

void ra_snes_addrlist_diag_dump(const void *map)
{
	if (!map || s_snes_addr_count == 0) return;
	const uint8_t *base = (const uint8_t *)map;

	// Dump VALCACHE response header
	const ra_val_resp_hdr_t *resp = (const ra_val_resp_hdr_t *)(base + RA_SNES_VALCACHE_OFFSET);
	RA_DBG("VALCACHE hdr: resp_id=%u resp_frame=%u (expect req_id=%u)",
		resp->response_id, resp->response_frame, s_snes_request_id);

	// Dump first 32 raw bytes from VALCACHE+8 (value area)
	const uint8_t *vals = base + RA_SNES_VALCACHE_OFFSET + 8;
	int dump_len = s_snes_addr_count < 32 ? s_snes_addr_count : 32;
	printf("\033[1;35mRA_MEM: VALCACHE raw[0..%d]: ", dump_len - 1);
	int non_zero = 0;
	for (int i = 0; i < dump_len; i++) {
		printf("%02X ", vals[i]);
		if (vals[i]) non_zero++;
	}
	printf("\033[0m\n");
	RA_DBG("VALCACHE: %d/%d non-zero in first %d bytes", non_zero, dump_len, dump_len);

	// Dump first 5 addresses for reference
	printf("\033[1;35mRA_MEM: Addrs[0..4]: ");
	for (int i = 0; i < 5 && i < s_snes_addr_count; i++) {
		printf("0x%05X ", s_snes_addrs[i]);
	}
	printf("\033[0m\n");

	// Dump ADDRLIST header as seen from DDRAM
	const ra_addr_req_hdr_t *ahdr = (const ra_addr_req_hdr_t *)(base + RA_SNES_ADDRLIST_OFFSET);
	RA_DBG("ADDRLIST hdr in DDRAM: count=%u req_id=%u", ahdr->addr_count, ahdr->request_id);
}
