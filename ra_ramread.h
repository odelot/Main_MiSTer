#ifndef RA_RAMREAD_H
#define RA_RAMREAD_H

#include <stdint.h>

// RetroAchievements RAM Mirror - Shared DDRAM Protocol
//
// This header defines the memory layout used by the FPGA core to expose
// emulated system RAM to the ARM/HPS side for RetroAchievements processing.
//
// The FPGA writes a snapshot of the emulated RAM to a known DDRAM address
// every VBlank. The ARM reads this snapshot to feed rcheevos.
//
// This protocol is console-agnostic. Each core writes the same header format,
// only differing in region count, sizes, and DDRAM base address.

// Magic number: "RACH" in little-endian
#define RA_MAGIC 0x52414348

// DDRAM base address for RA RAM mirror (physical address accessible via /dev/mem)
// The FPGA writes to DDRAM at 0x3A00000, which maps to physical address:
//   0x30000000 (DDRAM base) + 0x3A00000 = 0x33A00000
// But from ARM side using fpga_mem(): 0x20000000 | 0x3A00000 = 0x23A00000
// Actually, the DDR3 on DE10-Nano is at physical 0x00000000-0x3FFFFFFF (1GB)
// and the FPGA DDRAM port maps to 0x30000000 + addr.
// ARM accesses DDR3 directly at the physical address.
//
// DDRAM address calculation:
// ARM physical = 0x30000000 + (FPGA_DWORD_addr * 4)
// FPGA_DWORD_addr for RA mirror = 0x3400000
// ARM physical = 0x30000000 + 0x0D000000 = 0x3D000000
// This sits below the savestate area (0x3E000000).
#define RA_DDRAM_PHYS_BASE  0x3D000000
#define RA_DDRAM_MAP_SIZE   0x00010000  // 64KB is more than enough for any console

// Header structure at offset 0x00 (written by FPGA)
typedef struct __attribute__((packed)) {
	uint32_t magic;          // 0x00: Must be RA_MAGIC (0x52414348)
	uint8_t  region_count;   // 0x04: Number of RAM regions
	uint8_t  flags;          // 0x05: Bit 0 = busy (transfer in progress)
	uint16_t reserved;       // 0x06: Reserved
	uint32_t frame_counter;  // 0x08: Increments each VBlank
	uint32_t reserved2;      // 0x0C: Reserved
} ra_header_t;

// Region descriptor at offset 0x10 + index * 8 (written by FPGA)
typedef struct __attribute__((packed)) {
	uint32_t sdram_addr;     // Source address in SDRAM (for reference)
	uint16_t size;           // Size in bytes
	uint16_t ddram_offset;   // Offset from DDRAM base where data starts
} ra_region_desc_t;

// Flag bits
#define RA_FLAG_BUSY 0x01

// Maximum regions supported
#define RA_MAX_REGIONS 4

// NES-specific region indices
#define RA_NES_CPURAM_REGION  0  // $0000-$07FF (2KB)
#define RA_NES_CARTRAM_REGION 1  // $6000-$7FFF (up to 8KB+)

// Helper: map the RA mirror DDRAM region and return pointer
// Returns NULL on failure. Caller must call ra_ramread_unmap() when done.
void *ra_ramread_map(void);
void  ra_ramread_unmap(void *map);

// Check if RA mirror is active (magic number matches)
int ra_ramread_active(const void *map);

// Get current frame counter (returns 0 if not active)
uint32_t ra_ramread_frame(const void *map);

// Check if FPGA is currently writing (busy flag)
int ra_ramread_busy(const void *map);

// Get pointer to region data. Returns NULL if region index is invalid.
// The returned pointer is valid as long as the map is valid.
const uint8_t *ra_ramread_region_data(const void *map, int region_index);

// Get region size
uint16_t ra_ramread_region_size(const void *map, int region_index);

// Read a byte from the mirrored RAM using NES CPU address space mapping.
// Handles mirror resolution ($0000-$1FFF -> $0000-$07FF) and CARTRAM ($6000-$7FFF).
// Returns the byte value, or 0 if address is not in a mirrored region.
uint8_t ra_ramread_nes_byte(const void *map, uint16_t nes_addr);

// Read multiple bytes from mirrored RAM. Used as rcheevos read_memory callback.
// Returns number of bytes read.
uint32_t ra_ramread_nes_read(const void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes);

// Print a full diagnostic dump of the DDRAM mirror state to stdout and optional log file.
// Includes: header validation, frame counter, region descriptors, hex dump of first bytes.
void ra_ramread_debug_dump(const void *map);

// Print a one-line status summary (for periodic logging)
void ra_ramread_debug_status(const void *map);

#endif // RA_RAMREAD_H
