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

int ra_ramread_get_core_version(const void *map, uint8_t *major, uint8_t *minor)
{
	if (major) *major = 0;
	if (minor) *minor = 0;
	if (!map) return 0;
	const ra_header_t *hdr = (const ra_header_t *)map;
	if (hdr->magic != RA_MAGIC || hdr->core_version == 0) return 0;
	if (major) *major = (hdr->core_version >> 8) & 0xFF;
	if (minor) *minor = hdr->core_version & 0xFF;
	return 1;
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

uint8_t ra_ramread_atari2600_byte(const void *map, uint16_t addr)
{
	// rcheevos maps Atari 2600 RIOT RAM as a 128-byte block starting at address 0.
	// This matches Stella2014's retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM) which
	// returns a direct pointer to the 128-byte RIOT RAM — so rcheevos address 0x00
	// = RIOT byte 0 = CPU $0080, and rcheevos address 0x4E = RIOT byte 0x4E = CPU $00CE.
	// The old check (addr & 0x0080) was wrong: it rejected all addresses below 0x80.
	if (addr < 128) {
		if (!map) return 0;
		const ra_header_t *hdr = (const ra_header_t *)map;
		if (hdr->magic != RA_MAGIC) return 0;

		// Bypass the region descriptor: ra_riot_mirror always writes RIOT data
		// at a fixed DDRAM offset of 0x100 with size 128.
		const uint8_t *data = (const uint8_t *)map + 0x100;
		return data[addr];
	}
	return 0;
}

uint32_t ra_ramread_atari2600_read(const void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	for (uint32_t i = 0; i < num_bytes; i++) {
		buffer[i] = ra_ramread_atari2600_byte(map, (uint16_t)(address + i));
	}
	return num_bytes;
}

// ---------------------------------------------------------------------------
// Atari 7800 -- 4 KB internal RAM (ram0 + ram1)
//
// Atari 7800 hardware memory map:
//   ram1 (2KB): physical 0x1800-0x1FFF -> BRAM index = addr & 0x7FF -> DDRAM+0x900
//   ram0 (2KB): physical 0x2000-0x27FF -> BRAM index = addr & 0x7FF -> DDRAM+0x100
//               mirrors   0x0040-0x00FF -> BRAM index = addr & 0x7FF -> DDRAM+0x100
//               mirrors   0x0140-0x01FF -> BRAM index = addr & 0x7FF -> DDRAM+0x100
//
// BRAM is indexed by AB[10:0] which equals addr & 0x7FF for all valid ranges.
// rcheevos achievement conditions use the actual hardware addresses.
// ---------------------------------------------------------------------------
uint8_t ra_ramread_atari7800_byte(const void *map, uint16_t addr)
{
	if (!map) return 0;
	const ra_header_t *hdr = (const ra_header_t *)map;
	if (hdr->magic != RA_MAGIC) return 0;
	// ram1: physical 0x1800-0x1FFF
	if (addr >= 0x1800 && addr <= 0x1FFF)
		return ((const uint8_t *)map + 0x900)[addr & 0x7FF];
	// ram0: physical 0x2000-0x27FF (main) and zero-page/stack mirrors.
	// RA addresses are plain CPU addresses (identity map in rcheevos console 51),
	// and the core decodes zero page $0040-$00FF / stack $0140-$01FF into the
	// same BRAM via AB[10:0] (Maria/control.sv), so no offset is needed here.
	// (A "-8"/"+1" skew observed in older tests came from a capture-latency bug
	// in ra_7800_mirror.sv, fixed on the FPGA side — not from this mapping.)
	if ((addr >= 0x2000 && addr <= 0x27FF) ||
	    (addr >= 0x0040 && addr <= 0x00FF) ||
	    (addr >= 0x0140 && addr <= 0x01FF)) {
		uint16_t bram_idx = addr & 0x7FF;
		return ((const uint8_t *)map + 0x100)[bram_idx];
	}
	return 0;
}

uint32_t ra_ramread_atari7800_read(const void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	for (uint32_t i = 0; i < num_bytes; i++) {
		buffer[i] = ra_ramread_atari7800_byte(map, (uint16_t)(address + i));
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
// Selective Address Reading (shared "SNES-style" addrlist)
// ======================================================================

static int s_addr_cmp(const void *a, const void *b)
{
	uint32_t va = *(const uint32_t *)a;
	uint32_t vb = *(const uint32_t *)b;
	return (va > vb) - (va < vb);
}

static uint32_t s_snes_addrs[RA_SNES_MAX_ADDRS];
static uint8_t  s_snes_addr_dyn[RA_SNES_MAX_ADDRS]; // 1 = added via add_dynamic (AddAddress target)
static int      s_snes_dyn_count = 0;               // how many entries have the dyn flag
static int      s_snes_addr_count = 0;
static uint32_t s_snes_request_id = 0;
static int      s_snes_collecting = 0;

// Active mapping snapshot: the address ordering the FPGA's VALCACHE currently
// follows (the list revision it last confirmed via response_id). All cached
// reads (read_cached/lookup_byte/contains) go through this snapshot, never
// through the live pending list: a list mutation (add_dynamic insert, prune,
// recollect) shifts indices in the pending list immediately, but the VALCACHE
// keeps the old ordering until the FPGA picks up the new request_id — indexing
// it with the new list would return the neighbouring address's byte. Static
// addresses therefore stay cache-served across every revision; only addresses
// not yet in the active snapshot fall back to rtquery.
static uint32_t s_active_addrs[RA_SNES_MAX_ADDRS];
static int      s_active_count = 0;
static uint32_t s_active_id = 0;   // request_id the active snapshot reflects (0 = none)

// The list exactly as last written to DDRAM (what s_snes_request_id refers
// to). The live pending list (s_snes_addrs) keeps mutating between publishes
// via add_dynamic, so promotion must copy from here, not from the pending
// list — otherwise unpublished inserts would shift the promoted ordering.
static uint32_t s_published_addrs[RA_SNES_MAX_ADDRS];
static int      s_published_count = 0;

static void s_publish_snapshot(void)
{
	memcpy(s_published_addrs, s_snes_addrs, s_snes_addr_count * sizeof(uint32_t));
	s_published_count = s_snes_addr_count;
}

// Tear-free sample of the FPGA response header.
//
// response_id and response_frame share one 64-bit word that the FPGA writes in
// a single DDRAM transaction, but the ARM read them as two independent 32-bit
// loads. When the FPGA's write landed between those loads the ARM paired the
// OLD response_id (so s_sync_active declined to promote) with the NEW
// response_frame (so the console handler processed the frame): rc_client then
// indexed a VALCACHE the FPGA had already rewritten in the new revision's
// ordering using the previous revision's address array. Every cached address
// returned a neighbour's byte for that whole frame — delta real, mem garbage,
// which is exactly what fires "value jumped" achievements. Only possible while
// the list is growing (a revision change is required), which is why it showed
// up as bursts of spurious unlocks during smart-cache growth.
//
// Sample the header once behind a seqlock on response_frame (the FPGA bumps it
// on every write) and serve sync/caught_up/response_frame from that one sample.
static uint32_t s_resp_id_seen    = 0;
static uint32_t s_resp_frame_seen = 0;
static int      s_resp_valid      = 0;

static void s_refresh_resp(const void *map)
{
	if (!map) return;
	const volatile ra_val_resp_hdr_t *resp = (const volatile ra_val_resp_hdr_t *)
		((const uint8_t *)map + RA_SNES_VALCACHE_OFFSET);

	uint32_t f0 = resp->response_frame;
	// Steady state: frame unchanged means the FPGA has not written the header
	// since the last sample, so response_id cannot have changed either. One
	// uncached load — cheaper than the two loads this replaces.
	if (s_resp_valid && f0 == s_resp_frame_seen) return;

	for (int retry = 0; retry < 8; retry++) {
		uint32_t id = resp->response_id;
		uint32_t f1 = resp->response_frame;
		if (f0 == f1) {
			s_resp_id_seen    = id;
			s_resp_frame_seen = f0;
			s_resp_valid      = 1;
			return;
		}
		f0 = f1;
	}
	// Header churning faster than we can read it (never observed in practice):
	// keep the previous sample. Costs a skipped frame, never a misaligned one.
}

// Promote published → active when the FPGA has confirmed the current revision.
// Revisions are serialized (see flush/prune), so response_id is either the
// active id (transition in flight, keep old mapping) or the current request_id
// (promote). Any other value means a wholesale list replacement raced the
// FPGA; end_collect handles that case by invalidating the active snapshot.
static void s_sync_active(const void *map)
{
	if (!map) return;
	s_refresh_resp(map);
	if (!s_resp_valid) return;
	uint32_t rid = s_resp_id_seen;
	if (rid == s_active_id || rid != s_snes_request_id) return;
	memcpy(s_active_addrs, s_published_addrs, s_published_count * sizeof(uint32_t));
	s_active_count = s_published_count;
	s_active_id = rid;
}

// 1 when the FPGA has confirmed the latest published list revision.
// Uses the same sample as s_sync_active: a caught_up that ran ahead of the
// promotion could publish a further revision the active snapshot never sees.
static int s_fpga_caught_up(const void *map)
{
	if (!map) return 0;
	s_refresh_resp(map);
	return s_resp_valid && s_resp_id_seen == s_snes_request_id;
}

// Smart Cache dynamic-add state (see "Smart Cache" section below)
static int s_dynamic_pending = 0;   // 1 if new addresses added since last flush
static int s_dynamic_added   = 0;   // count of addresses added this cycle
static int s_flush_defer_streak = 0; // consecutive freshness-gate deferrals

// Publish-phase tracking. response_frame() is the once-per-poll call every
// handler makes, so two consecutive calls bracket WHEN the FPGA wrote the
// response header: it landed between the previous call (which still returned
// the old frame) and the first call returning the new one. That gives an
// upper bound on how stale our knowledge of the frame phase is, which the
// freshness gate (s_publish_window_open) uses to prove a publish cannot
// collide with the next scan (closes the check-then-act window left by the
// busy-flag test alone).
static struct timespec s_rf_prev_call  = {0, 0};
static struct timespec s_frame_seen_at = {0, 0};
static uint32_t s_frame_staleness_us = 0xFFFFFFFFu;  // bound on detection lag
static uint32_t s_rf_last_returned = 0;

#define COLLECT_BUF_MAX (RA_SNES_MAX_ADDRS * 4)
static uint32_t s_collect_buf[COLLECT_BUF_MAX];
static int      s_collect_count = 0;

void ra_snes_addrlist_init(void)
{
	s_snes_addr_count = 0;
	// s_snes_request_id is intentionally NOT reset here: it must stay monotonic
	// for the lifetime of the process. On a core reset the FPGA can complete an
	// in-flight VBlank scan (started before the reset pulse) and write a stale
	// response header AFTER the ARM cleared the mirror. If request IDs restarted
	// at 1, that stale response_id could match the fresh bootstrap request and
	// poison last_resp_frame with a huge pre-reset frame counter, stalling
	// achievement tracking until the counter catches up (hours).
	s_snes_collecting = 0;
	s_collect_count = 0;
	s_dynamic_pending = 0;
	s_dynamic_added = 0;
	s_snes_dyn_count = 0;
	s_active_count = 0;
	s_active_id = 0;
	s_published_count = 0;
	s_resp_id_seen = 0;
	s_resp_frame_seen = 0;
	s_resp_valid = 0;
	s_flush_defer_streak = 0;
	s_rf_last_returned = 0;
	s_frame_staleness_us = 0xFFFFFFFFu;
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

	// A wholesale replacement while a previous revision is still in flight
	// would leave the VALCACHE ordered by a revision we no longer have a
	// snapshot of. Invalidate the active snapshot in that (rare) case so
	// reads miss to rtquery/zero instead of misaligning; the next confirmed
	// response re-promotes. When the FPGA is caught up (the normal case) the
	// active snapshot stays valid until the new revision is confirmed.
	if (s_active_id && !s_fpga_caught_up(map)) {
		s_active_count = 0;
		s_active_id = 0;
	}

	// Update local list — a full collect defines the STATIC baseline
	memcpy(s_snes_addrs, s_collect_buf, new_count * sizeof(uint32_t));
	memset(s_snes_addr_dyn, 0, (size_t)new_count);
	s_snes_dyn_count = 0;
	s_snes_addr_count = new_count;
	s_snes_request_id++;
	s_publish_snapshot();

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
	if (!map || s_active_count == 0) return 0;

	// Binary search over the ACTIVE snapshot — the ordering the VALCACHE
	// actually follows (see s_sync_active).
	int lo = 0, hi = s_active_count - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (s_active_addrs[mid] == addr) {
			const uint8_t *vals = (const uint8_t *)map + RA_SNES_VALCACHE_OFFSET + 8;
			return vals[mid];
		}
		if (s_active_addrs[mid] < addr) lo = mid + 1;
		else hi = mid - 1;
	}
	return 0;
}

int ra_snes_addrlist_is_ready(const void *map)
{
	if (!map || s_snes_addr_count == 0 || s_snes_request_id == 0) return 0;
	s_sync_active(map);
	return s_fpga_caught_up(map);
}

int ra_snes_addrlist_count(void)
{
	return s_snes_addr_count;
}

const uint32_t *ra_snes_addrlist_addrs(void)
{
	return s_snes_addrs;
}

// Active (FPGA-confirmed) snapshot — the ordering the VALCACHE follows.
// Use these when pairing addresses with VALCACHE values by index; the
// pending accessors above may already contain unpublished insertions.
int ra_snes_addrlist_active_count(void)
{
	return s_active_count;
}

const uint32_t *ra_snes_addrlist_active_addrs(void)
{
	return s_active_addrs;
}

uint32_t ra_snes_addrlist_request_id(void)
{
	return s_snes_request_id;
}

static uint32_t s_ts_diff_us(const struct timespec *a, const struct timespec *b)
{
	int64_t us = (int64_t)(a->tv_sec - b->tv_sec) * 1000000
	           + (a->tv_nsec - b->tv_nsec) / 1000;
	if (us < 0) return 0;
	if (us > 0xFFFFFFF0LL) return 0xFFFFFFF0u;
	return (uint32_t)us;
}

uint32_t ra_snes_addrlist_response_frame(const void *map)
{
	if (!map) return 0;
	// Called once per poll by every Selective Address console — piggyback the
	// pending→active promotion check here so no per-console change is needed.
	// The frame returned must come from the same header sample the promotion
	// decision used (see s_refresh_resp), never from a fresh load.
	s_sync_active(map);
	uint32_t frame = s_resp_valid ? s_resp_frame_seen : 0;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (frame != s_rf_last_returned) {
		s_frame_seen_at = now;
		s_frame_staleness_us = (s_rf_prev_call.tv_sec | s_rf_prev_call.tv_nsec)
			? s_ts_diff_us(&now, &s_rf_prev_call) : 0xFFFFFFFFu;
		s_rf_last_returned = frame;
	}
	s_rf_prev_call = now;
	return frame;
}

// 1 when a publish provably fits before the next scan: the header write
// happened at most (elapsed + staleness bound) ago, the next scan starts a
// full frame period (>= 16.6ms NTSC / 20ms PAL) after it, and the memcpy
// takes well under 1ms — an 8ms budget leaves >= 7ms of hard margin.
static int s_publish_window_open(void)
{
	if (s_frame_staleness_us == 0xFFFFFFFFu) return 0;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	uint32_t since = s_ts_diff_us(&now, &s_frame_seen_at);
	return (since + s_frame_staleness_us) < 8000;
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

// ======================================================================
// Smart Cache: incremental address management
// (s_dynamic_pending / s_dynamic_added are declared next to the addrlist
//  state above so ra_snes_addrlist_init can clear them.)
// ======================================================================

// "Servable from cache" check — searches the ACTIVE snapshot, not the pending
// list: an address inserted by add_dynamic must keep missing (and thus be
// served by rtquery) until the FPGA confirms the revision that contains it.
int ra_snes_addrlist_contains(uint32_t addr)
{
	if (s_active_count == 0) return -1;
	int lo = 0, hi = s_active_count - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (s_active_addrs[mid] == addr) return mid;
		if (s_active_addrs[mid] < addr) lo = mid + 1;
		else hi = mid - 1;
	}
	return -1;
}

// One binary search returns both index match flag and the cached value byte.
// Replaces back-to-back contains()+read_cached() in PSX smart_cache hot path,
// halving the per-read CPU cost (critical to keep ARM frame budget under the
// threshold that starves CD-ROM XA streaming and causes audio glitches).
uint8_t ra_snes_addrlist_lookup_byte(const void *map, uint32_t addr, int *hit)
{
	if (!map || s_active_count == 0) {
		if (hit) *hit = 0;
		return 0;
	}
	int lo = 0, hi = s_active_count - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (s_active_addrs[mid] == addr) {
			const uint8_t *vals = (const uint8_t *)map + RA_SNES_VALCACHE_OFFSET + 8;
			if (hit) *hit = 1;
			return vals[mid];
		}
		if (s_active_addrs[mid] < addr) lo = mid + 1;
		else hi = mid - 1;
	}
	if (hit) *hit = 0;
	return 0;
}

int ra_snes_addrlist_add_dynamic(uint32_t addr)
{
	// Check if already in the list
	if (s_snes_addr_count > 0) {
		int lo = 0, hi = s_snes_addr_count - 1;
		int insert_pos = s_snes_addr_count; // default: append at end
		while (lo <= hi) {
			int mid = (lo + hi) / 2;
			if (s_snes_addrs[mid] == addr) return 0; // already exists
			if (s_snes_addrs[mid] < addr) lo = mid + 1;
			else { insert_pos = mid; hi = mid - 1; }
		}
		if (lo < s_snes_addr_count && s_snes_addrs[lo] > addr)
			insert_pos = lo;
		if (s_snes_addr_count >= RA_SNES_MAX_ADDRS) return 0; // list full

		// Insert at sorted position (dyn flags move in lockstep)
		if (insert_pos < s_snes_addr_count) {
			memmove(&s_snes_addrs[insert_pos + 1],
				&s_snes_addrs[insert_pos],
				(s_snes_addr_count - insert_pos) * sizeof(uint32_t));
			memmove(&s_snes_addr_dyn[insert_pos + 1],
				&s_snes_addr_dyn[insert_pos],
				(size_t)(s_snes_addr_count - insert_pos));
		}
		s_snes_addrs[insert_pos] = addr;
		s_snes_addr_dyn[insert_pos] = 1;
	} else {
		if (s_snes_addr_count >= RA_SNES_MAX_ADDRS) return 0;
		s_snes_addrs[0] = addr;
		s_snes_addr_dyn[0] = 1;
	}
	s_snes_addr_count++;
	s_snes_dyn_count++;
	s_dynamic_pending = 1;
	s_dynamic_added++;
	return 1;
}

int ra_snes_addrlist_has_pending(void)
{
	return s_dynamic_pending;
}

int ra_snes_addrlist_flush_dynamic(void *map)
{
	if (!s_dynamic_pending || !map) return 0;
	// Serialize revisions: publish only when the FPGA has confirmed the
	// current one. This guarantees the VALCACHE ordering is always either
	// the active snapshot or the (single) revision in flight, so cached
	// reads never misalign. Deferred flushes keep s_dynamic_pending set and
	// retry next frame; the pending addresses are served by rtquery meanwhile.
	if (!s_fpga_caught_up(map)) return 0;
	// Never publish while the FPGA is mid-scan (busy set at vblank, cleared
	// after the response header): S_READ_PAIR re-reads the address array
	// incrementally DURING the scan, so a memcpy landing mid-scan makes the
	// FPGA serve a mix of two orderings under the OLD response_id — which the
	// ARM cannot detect. Deferring one frame costs nothing: the pending
	// addresses keep being served by rtquery until published (same as the
	// add→flush gap today).
	if (ra_ramread_busy(map)) return 0;
	// Freshness gate: busy alone is check-then-act — a vblank can still land
	// during the memcpy below. Publish only when the phase argument holds
	// (see s_publish_window_open); the residual then rounds to zero instead
	// of ~0.2%/flush. If the ARM is persistently late the gate would starve
	// list growth, so after 30 consecutive deferrals (~0.5s) fall back to
	// busy-check-only — the is_ready net downstream still catches that case
	// as one skipped eval tick.
	if (!s_publish_window_open()) {
		if (++s_flush_defer_streak < 30) return 0;
		RA_DBG("SmartCache flush: freshness fallback after %d deferrals", s_flush_defer_streak);
	}
	s_flush_defer_streak = 0;
	s_dynamic_pending = 0;
	int flushed = s_dynamic_added;
	s_dynamic_added = 0;

	// Bump request ID and write entire list to DDRAM
	s_snes_request_id++;
	s_publish_snapshot();
	uint8_t *base = (uint8_t *)map;

	// Write addresses first
	uint32_t *addrs = (uint32_t *)(base + RA_SNES_ADDRLIST_OFFSET + 8);
	memcpy(addrs, s_snes_addrs, s_snes_addr_count * sizeof(uint32_t));
	__sync_synchronize();

	// Write header
	ra_addr_req_hdr_t *hdr = (ra_addr_req_hdr_t *)(base + RA_SNES_ADDRLIST_OFFSET);
	hdr->addr_count = s_snes_addr_count;
	__sync_synchronize();
	hdr->request_id = s_snes_request_id;
	__sync_synchronize();

	RA_DBG("SmartCache flush: %d new addrs, total=%d, request_id=%u",
		flushed, s_snes_addr_count, s_snes_request_id);
	return 1;
}

int ra_snes_addrlist_dynamic_count(void)
{
	return s_dynamic_added;
}

int ra_snes_addrlist_dyn_count(void)
{
	return s_snes_dyn_count;
}

// Dynamic-only cleanup: drop every address added via add_dynamic (AddAddress
// pointer targets), keeping the static bootstrap set untouched. Safe by
// construction: a pruned address that is still needed simply misses the cache
// on the next frame, is answered by rtquery and re-added. Much cheaper than a
// full re-collect (no extra rc_client_do_frame pass, no qsort).
// Returns the number of entries removed (0 = nothing to do, list unchanged).
int ra_snes_addrlist_prune_dynamic(void *map)
{
	if (!s_snes_dyn_count) return 0;
	// Never prune down to an empty list (all-dynamic lists would break the
	// is_ready/reindex handshake); keep everything instead.
	if (s_snes_dyn_count >= s_snes_addr_count) return 0;
	// Same revision serialization as flush_dynamic: defer until the FPGA
	// confirmed the current list, so the active snapshot stays the only
	// other ordering in play. Callers retry on a later cleanup tick.
	if (map && !s_fpga_caught_up(map)) return 0;
	// Same mid-scan publish guards as flush_dynamic (see comments there).
	// No fallback here: a deferred prune just waits for a later cleanup tick.
	if (map && ra_ramread_busy(map)) return 0;
	if (map && !s_publish_window_open()) return 0;

	int w = 0;
	for (int i = 0; i < s_snes_addr_count; i++) {
		if (!s_snes_addr_dyn[i]) {
			s_snes_addrs[w] = s_snes_addrs[i];
			s_snes_addr_dyn[w] = 0;
			w++;
		}
	}
	int removed = s_snes_addr_count - w;
	s_snes_addr_count = w;
	s_snes_dyn_count = 0;
	s_dynamic_pending = 0;
	s_dynamic_added = 0;

	// Publish the shrunk list (same commit order as flush_dynamic)
	s_snes_request_id++;
	s_publish_snapshot();
	if (map) {
		uint8_t *base = (uint8_t *)map;
		uint32_t *addrs = (uint32_t *)(base + RA_SNES_ADDRLIST_OFFSET + 8);
		memcpy(addrs, s_snes_addrs, s_snes_addr_count * sizeof(uint32_t));
		__sync_synchronize();
		ra_addr_req_hdr_t *hdr = (ra_addr_req_hdr_t *)(base + RA_SNES_ADDRLIST_OFFSET);
		hdr->addr_count = s_snes_addr_count;
		__sync_synchronize();
		hdr->request_id = s_snes_request_id;
		__sync_synchronize();
	}

	RA_DBG("DynPrune: removed %d dynamic addrs, %d static kept, request_id=%u",
		removed, s_snes_addr_count, s_snes_request_id);
	return removed;
}

// ======================================================================
// Realtime Query Mailbox (Selective Address "on steroids")
// ======================================================================

static uint8_t s_rtquery_seq = 0;

// Busy-wait budget: iterations to spin waiting for the FPGA response.
// Default keeps the historical ~100k (worst case tens of ms). Handlers can
// lower it (e.g. justifier_test caps it to ~2k ≈ 0.5-1ms) so a mailbox
// timeout can never hold the main loop long enough to lag input forwarding.
static int s_rtquery_spin_limit = 100000;

// Cooldown after a timeout: skip further queries briefly instead of
// re-spinning on every read (one dead mailbox must not become a stall storm).
static struct timespec s_rtq_cooldown = {0, 0};

void ra_rtquery_set_spin_limit(int iters)
{
	s_rtquery_spin_limit = (iters > 0) ? iters : 100000;
	RA_DBG("RTQuery: spin limit set to %d iterations", s_rtquery_spin_limit);
}

// Wall-time cap for the busy-wait (0 = iteration limit only).
static uint32_t s_rtquery_timeout_us = 0;
static int s_rtq_last_failed = 0;
static ra_rtquery_stats_t s_rtq_stats = {};

void ra_rtquery_set_timeout_us(uint32_t us)
{
	s_rtquery_timeout_us = us;
	RA_DBG("RTQuery: timeout set to %u us", us);
}

int ra_rtquery_last_failed(void)
{
	return s_rtq_last_failed;
}

void ra_rtquery_get_stats(ra_rtquery_stats_t *out, int reset)
{
	if (out) *out = s_rtq_stats;
	if (reset) memset(&s_rtq_stats, 0, sizeof(s_rtq_stats));
}

static inline uint64_t rtq_now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void ra_rtquery_init(void *map)
{
        // s_rtquery_seq is intentionally NOT reset: it keeps counting (mod 256,
        // skipping 0) across re-inits. Restarting at 1 after a core reset could
        // collide with the sequence number the FPGA last latched, making it
        // ignore the request and costing a full 100ms busy-wait timeout.
        if (!map) return;

        // Clear the control word so FPGA sees no pending request
        uint8_t *base = (uint8_t *)map;
        ra_query_ctrl_t *ctrl = (ra_query_ctrl_t *)(base + RA_QUERY_CTRL_OFFSET);
        memset(ctrl, 0, sizeof(*ctrl));
        __sync_synchronize();

        // Signal FPGA that rtquery polling is now needed.
        // The FPGA reads RA_ARM_CONFIG_OFFSET once per VBlank and starts polling
        // the query mailbox only when this bit is set. This prevents ~107k unnecessary
        // DDRAM reads per second on cores where rtquery is enabled but not actively used.
        base[RA_ARM_CONFIG_OFFSET] |= RA_ARM_CFG_RTQUERY;
        __sync_synchronize();

        RA_DBG("RTQuery: initialized (mailbox at offset 0x%X)", RA_QUERY_CTRL_OFFSET);
}

void ra_rtquery_disable(void *map)
{
        if (!map) return;
        uint8_t *base = (uint8_t *)map;
        base[RA_ARM_CONFIG_OFFSET] &= (uint8_t)(~RA_ARM_CFG_RTQUERY);
        __sync_synchronize();
        RA_DBG("RTQuery: disabled (FPGA will stop polling query mailbox after next VBlank)");
}

void ra_clear_en_set(void *map)
{
        if (!map) return;
        uint8_t *base = (uint8_t *)map;
        base[RA_ARM_CONFIG_OFFSET] |= RA_ARM_CFG_CLEAR_EN;
        __sync_synchronize();
}

void ra_clear_en_clear(void *map)
{
        if (!map) return;
        uint8_t *base = (uint8_t *)map;
        base[RA_ARM_CONFIG_OFFSET] &= (uint8_t)(~RA_ARM_CFG_CLEAR_EN);
        __sync_synchronize();
}

uint32_t ra_rtquery_read(void *map, uint32_t address, uint32_t num_bytes)
{
        s_rtq_last_failed = 1;
        if (!map || num_bytes == 0 || num_bytes > 4) return 0;

        // Inside a post-timeout cooldown window: fail fast (returns 0, which
        // the smart cache treats as a plain miss).
        if (s_rtq_cooldown.tv_sec) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                if (now.tv_sec < s_rtq_cooldown.tv_sec ||
                    (now.tv_sec == s_rtq_cooldown.tv_sec && now.tv_nsec < s_rtq_cooldown.tv_nsec)) {
                        s_rtq_stats.failed++;
                        return 0;
                }
                s_rtq_cooldown.tv_sec = 0;
                s_rtq_cooldown.tv_nsec = 0;
        }

        uint8_t *base = (uint8_t *)map;
        ra_query_ctrl_t *ctrl = (ra_query_ctrl_t *)(base + RA_QUERY_CTRL_OFFSET);
        ra_query_req_t  *req  = (ra_query_req_t *)(base + RA_QUERY_REQ_OFFSET);
        ra_query_resp_t *resp = (ra_query_resp_t *)(base + RA_QUERY_RESP_OFFSET);

        // Fill single query slot
        req[0].address   = address;
        req[0].num_bytes = (uint8_t)num_bytes;
        __sync_synchronize();

        // Increment sequence and write control
        s_rtquery_seq++;
        if (s_rtquery_seq == 0) s_rtquery_seq = 1;  // Never use 0

        ctrl->num_queries = 1;
        ctrl->request_seq = s_rtquery_seq;
        __sync_synchronize();

        // Busy-wait for FPGA response (typically 5-50µs). With a wall-time
        // cap set, the clock is sampled every 32 spins (the iteration limit
        // still applies as a backstop).
        volatile ra_query_ctrl_t *vctrl = (volatile ra_query_ctrl_t *)ctrl;
        int timeout = s_rtquery_spin_limit;
        uint64_t t_start = rtq_now_us();
        uint64_t deadline = s_rtquery_timeout_us ? t_start + s_rtquery_timeout_us : 0;
        while (vctrl->response_seq != s_rtquery_seq && --timeout > 0) {
                if (deadline && !(timeout & 31) && rtq_now_us() >= deadline) {
                        timeout = 0;
                        break;
                }
        }

        uint32_t waited_us = (uint32_t)(rtq_now_us() - t_start);
        s_rtq_stats.total_us += waited_us;
        if (waited_us > s_rtq_stats.max_us) s_rtq_stats.max_us = waited_us;

        if (timeout <= 0) {
                s_rtq_stats.failed++;
                // Timeout — FPGA didn't respond. Enter a 20ms cooldown so the
                // main loop is not held by back-to-back dead queries.
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                now.tv_nsec += 20000000L;
                if (now.tv_nsec >= 1000000000L) { now.tv_sec++; now.tv_nsec -= 1000000000L; }
                s_rtq_cooldown = now;
                return 0;
        }

        // Read result
        volatile ra_query_resp_t *vresp = (volatile ra_query_resp_t *)resp;
        uint32_t value = vresp[0].value;

        // Mask to requested byte count
        if (num_bytes < 4) {
                value &= (1u << (num_bytes * 8)) - 1;
        }

        s_rtq_stats.count++;
        s_rtq_last_failed = 0;
        return value;
}

int ra_rtquery_supported(const void *map)
{
        // Check FPGA version in debug word at DDRAM_BASE + 0x10
        // Debug word 1: {ver(8), dispatch_cnt(8), first_dout(16), timeout_cnt(16), ok_cnt(16)}
        // ver is at byte offset 0x17 (top byte of the 64-bit word at offset 0x10)
        if (!map) return 0;
        const uint8_t *base = (const uint8_t *)map;
        // The debug word is at DDRAM offset 0x10 (word index 2)
        // In the 64-bit word: FPGA_VERSION is in bits [63:56] = byte[7] of the word
        // At byte offset 0x10 + 7 = 0x17
        uint8_t ver = base[0x17];
        return ver >= 0x02;
}

