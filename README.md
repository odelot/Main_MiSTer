# Main_MiSTer — RetroAchievements Fork

This is a fork of the official [MiSTer Main binary](https://github.com/MiSTer-devel/Main_MiSTer) with **RetroAchievements** support for MiSTer FPGA.

> **Status:** Experimental / Proof of Concept

## Supported Cores

| Core | Console ID | Modified Core Repo |
|------|-----------|--------------------|
| NES | 7 | [odelot/NES_MiSTer](https://github.com/odelot/NES_MiSTer) |
| SNES | 3 | [odelot/SNES_MiSTer](https://github.com/odelot/SNES_MiSTer) |
| Genesis / Mega Drive | 1 | [odelot/MegaDrive_MiSTer](https://github.com/odelot/MegaDrive_MiSTer) |
| Master System / Game Gear | 11 | [odelot/SMS_MiSTer](https://github.com/odelot/SMS_MiSTer) |
| Gameboy / Gameboy Color | 4 | [odelot/Gameboy_MiSTer](https://github.com/odelot/Gameboy_MiSTer) |
| N64 | 2 | [odelot/N64_MiSTer](https://github.com/odelot/N64_MiSTer) |
| PSX | 12 | [odelot/PSX_MiSTer](https://github.com/odelot/PSX_MiSTer) |
| GBA (Game Boy Advance) | 5 | [odelot/GBA_MiSTer](https://github.com/odelot/GBA_MiSTer) |
| Mega CD / Sega CD | 9 | [odelot/MegaCD_MiSTer](https://github.com/odelot/MegaCD_MiSTer) |
| NeoGeo (MVS / AES / CD) | 27 | [odelot/NeoGeo_MiSTer](https://github.com/odelot/NeoGeo_MiSTer) |
| TurboGrafx-16 / PC Engine | 8 | [odelot/TurboGrafx16_MiSTer](https://github.com/odelot/TurboGrafx16_MiSTer) |
| Atari 2600 (via Atari7800 core) | 25 | [odelot/Atari7800_MiSTer](https://github.com/odelot/Atari7800_MiSTer) |
| Sega 32X | 10 | [odelot/S32X_MiSTer](https://github.com/odelot/S32X_MiSTer) |

## How to Test

Pre-built binaries are available on the [Releases](https://github.com/odelot/Main_MiSTer/releases) page — no compilation needed.

1. Download the latest release and extract all files.
2. Edit `retroachievements.cfg` with your [RetroAchievements](https://retroachievements.org/) credentials (username and password).
3. Copy all files to `/media/fat` on your MiSTer SD card.
4. You will also need the modified core for the console you want to play. See the table above for the corresponding core repo (each one has its own release binaries).

For example, for **Master System / Game Gear**, get the modified SMS core from [odelot/SMS_MiSTer](https://github.com/odelot/SMS_MiSTer).

## What's Different from the Original

The [upstream Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer) binary manages cores, user input, video output, and the OSD menu on the MiSTer platform. This fork adds a full RetroAchievements integration layer on top, without modifying any existing core functionality.

### Files Added

| File | Purpose |
|------|--------|
| `achievements.cpp / .h` | Core lifecycle — init, load game, per-frame poll, unload, OSD popups |
| `achievements_<console>.cpp` | Per-console handlers (NES, SNES, Genesis, MegaCD, SMS, Gameboy, GBA, N64, PSX, NeoGeo, Atari 2600, Sega 32X) |
| `achievements_console.h` | Console handler interface (`console_handler_t` struct) |
| `achievements_console_lookup.cpp` | Dispatch table mapping core names to console handlers |
| `ra_http.cpp / .h` | Async HTTP worker thread that executes RetroAchievements API calls via `curl` |
| `ra_ramread.cpp / .h` | Reads emulated console RAM from the DDRAM mirror written by the FPGA core |
| `ra_cdreader_chd.cpp / .h` | Unified CD reader bridge: `.chd` via MiSTer's `libchdr`, `.cue`/`.gdi` via the rcheevos default handler. Registered at startup, enabling CHD disc support for all disc-based consoles (PSX, Mega CD, PCE-CD, NeoGeo CD). |
| `shmem.cpp / .h` | Thin wrapper around `/dev/mem` + `mmap` for ARM ↔ FPGA shared memory access |
| `retroachievements.cfg` | User credentials and options (username, password, hardcore mode, leaderboards, popup settings, debug) |
| `lib/rcheevos/` | The [rcheevos](https://github.com/RetroAchievements/rcheevos) library (achievement logic, server protocol) |

### Files Modified (hooks into the main loop)

| File | Change |
|------|--------|
| `scheduler.cpp` | Calls `achievements_poll()` every frame |
| `menu.cpp` | Calls `achievements_load_game()` when a ROM is selected |
| `main.cpp` | Calls `achievements_init()` / `achievements_deinit()` at startup/shutdown |
| `user_io.cpp` | Notifies the RA layer when a core reset is triggered (keyboard or joystick combo) |
| `Makefile` | Conditionally compiles rcheevos sources and links against them |

No other existing behavior is changed.

## Architecture

The RetroAchievements integration uses a four-layer pipeline that connects the FPGA hardware to the RetroAchievements server:

```
┌───────────────────────────────────────────────────────┐
│  FPGA Core (NES / SNES / Genesis / SMS / GB / GBA / N64 / PSX / MCD / NeoGeo / TG16 / Atari2600 / S32X) │
│  ra_ram_mirror*.sv exposes emulated RAM to DDRAM      │
│  every VBlank (~60 Hz)                                │
└──────────────────────┬────────────────────────────────┘
                       │  DDRAM at physical 0x3D000000
                       ▼
┌───────────────────────────────────────────────────────┐
│  ARM Binary (this repo)                               │
│  shmem.cpp  — mmap /dev/mem to read DDRAM             │
│  ra_ramread — parse header, map console addresses     │
└──────────────────────┬────────────────────────────────┘
                       │
                       ▼
┌───────────────────────────────────────────────────────┐
│  rcheevos SDK                                         │
│  Evaluates achievement conditions against RAM         │
│  Manages unlock state, leaderboards, progress         │
└──────────────────────┬────────────────────────────────┘
                       │  Async HTTP (ra_http worker)
                       ▼
┌───────────────────────────────────────────────────────┐
│  RetroAchievements Server                             │
│  Login, game identification, unlock reporting         │
└───────────────────────────────────────────────────────┘
```

### How It Works

1. **Init** — On startup, the ARM binary maps the DDRAM mirror region, starts an HTTP worker thread, and loads credentials from `retroachievements.cfg`.
2. **Load Game** — When a ROM is selected in the MiSTer menu, the binary checks internet connectivity (DNS + non-blocking TCP probe to `retroachievements.org`). If online and not yet logged in, it logs in to RetroAchievements on the spot, then computes the ROM's MD5 hash and identifies the game against the RA database. If no internet is detected, an OSD message is shown and login is deferred. Loading the **standard community core** (which lacks the RA mirror module) silently suppresses all RA activity — no spurious login or network calls are made.
3. **Per-Frame Poll** — Every frame (~60 Hz), `achievements_poll()` checks whether the FPGA has written new data. If a new frame is available, it calls `rc_client_do_frame()` from the rcheevos library, which evaluates achievement conditions against the current RAM state.
4. **Unlock / Notify** — When an achievement triggers, the event handler displays an OSD notification and optionally plays a sound (`/media/fat/achievement.wav`). The unlock is reported to the RA server asynchronously. Leaderboard events (start, fail, submit, scoreboard result) also show OSD notifications; a tracker display updates in real time while a leaderboard is active.
5. **Core Reset** — When the user triggers a core reset (keyboard shortcut or joystick combo), the RA layer is notified so the rcheevos client can reset its internal state correctly.
6. **Unload / Shutdown** — State is cleaned up when switching cores or shutting down.

> Achievements default to **softcore mode** (savestates allowed).

### Achievement List (F6)

While the OSD is open and a game with achievements is loaded, press **F6** to open a scrollable achievement list for the current game. The list shows all core achievements grouped with **unlocked entries first** (marked `►`) followed by **locked entries** (marked `◄`). The OSD title displays the total count, e.g. *Achievements (42)*.

Navigation inside the list:

| Key | Action |
|-----|--------|
| ↑ / ↓ | Move one entry |
| Page Up / ← | Previous page |
| Page Down / → | Next page |
| Home | Jump to first entry |
| End | Jump to last entry |
| Menu button | Close and return to normal OSD |

### DDRAM Mirror Layout

Every supported core writes a structured block at ARM physical address `0x3D000000`:

| Offset | Content |
|--------|---------|
| `0x00000` | Header: magic `"RACH"`, region count, flags (busy bit), frame counter |
| `0x00100+` | RAM data area (layout varies per core — used by Full Mirror protocol) |
| `0x40000` | Address request list — ARM writes here (Selective Address protocol) |
| `0x48000` | Value response cache — FPGA writes results here (Selective Address protocol) |

### Per-Core RAM Exposure Strategy

There are two protocols for exposing emulated RAM to the ARM:

- **Full Mirror** — The FPGA copies all relevant RAM to DDRAM every VBlank. Simple but only viable for small RAM spaces.
- **Selective Address (Option C)** — The ARM writes a list of addresses it needs; the FPGA reads only those values and writes them back. Required for cores with large address spaces.

#### NES — Full Mirror (`ra_ram_mirror.sv`)
The FPGA copies all relevant RAM to DDRAM on every VBlank (~10 KB total):
- **CPU-RAM** ($0000–$07FF) — 2 KB
- **Cart SRAM** ($6000–$7FFF) — 8 KB

#### SNES — Selective Address (`ra_ram_mirror_snes.sv`)
Due to the larger RAM space, the ARM binary writes the list of addresses it needs, and the FPGA reads only those (~185 per frame):
- **WRAM** ($000000–$01FFFF) — 128 KB
- **BSRAM** ($020000+) — up to 256 KB

#### Genesis / Mega Drive — Selective Address (`ra_ram_mirror_md.sv`)
Same request/response protocol. The FPGA reads requested addresses directly from the 68K Work RAM BRAM:
- **68K Work RAM** ($000000–$00FFFF) — 64 KB
- Includes hardware-accurate FC1004 address bit-13 inversion for correct BRAM mapping

#### GBA (Game Boy Advance) — Selective Address (`ra_ram_mirror_gba.sv`)
Same request/response protocol. The FPGA reads from three distinct memory backends:
- **IWRAM** ($00000–$07FFF) — 32 KB (on-chip block RAM, Port B read with double-read collision detection)
- **EWRAM** ($08000–$47FFF) — 256 KB (DDRAM, via `Softmap_GBA_WRam_ADDR`)
- **Cart RAM / Flash** ($48000–$57FFF) — up to 64 KB (DDRAM, via `Softmap_GBA_FLASH_ADDR`)

The GBA core's existing `ddram.sv` module was extended with a dedicated lowest-priority RA channel for both reads and writes. No separate arbiter file is needed — the RA port is integrated directly into the DDRAM controller. IWRAM reads use the BRAM Port B of `gba_memorymux`, with a read-verify-retry mechanism (up to 4 retries) to handle occasional collisions when the CPU writes on Port A at the same cycle.

A unique GBA-specific feature: the ARM binary pre-fills the Flash DDRAM region (128 KB at 0x30000000) with `0xFF` on game load when no save file is present, since real GBA Flash reads `0xFF` when erased and many RA conditions check for this sentinel value. Total exposed: **352 KB**.

#### Mega CD / Sega CD — Selective Address (`ra_ram_mirror_mcd.sv`)
Same request/response protocol. The FPGA reads from SDRAM (not BRAM), handling two distinct memory regions:
- **68K Work RAM** ($00000–$0FFFF) — 64 KB (SDRAM bank 1)
- **MCD Program RAM** ($10000–$8FFFF) — 512 KB (SDRAM banks 0/1 with bank switching)

Unlike the MegaDrive core, no DDRAM arbitration is needed (`ddram_ra_mcd.sv` is a simple pass-through) because the Mega CD core has no other DDRAM consumer. Total exposed: **576 KB**.

#### Master System / Game Gear — Selective Address (`ra_ram_mirror_sms.sv`)
Uses the same selective address protocol. The FPGA reads from dual-ported System RAM and NVRAM:
- **System RAM** ($0000–$1FFF) — 8 KB (Z80 $C000–$DFFF mirrored)
- **NVRAM / Cart RAM** ($2000–$9FFF) — up to 32 KB

The SMS core converts its existing single-port RAMs to dual-port (`dpram`) so the RA mirror can read on Port B without disturbing the CPU on Port A. A custom DDRAM arbiter (`ddram_arb_sms.sv`) shares bus access between the framebuffer and the RA mirror.

#### Gameboy / Gameboy Color — Selective Address (`ra_ram_mirror_gb.sv`)
Uses the selective address protocol. The FPGA reads from dual-ported WRAM and ZPRAM (HRAM), plus cart RAM via a dedicated SDRAM channel:
- **WRAM** ($C000–$DFFF + GBC banks 2–7 via $10000–$15FFF) — up to 32 KB
- **Cart RAM** ($A000–$BFFF + banks 1–15 via $16000–$33FFF) — up to 128 KB via SDRAM ch2
- **HRAM / ZPRAM** ($FF80–$FFFE) — 127 bytes
- **Echo RAM** ($E000–$FDFF) — automatically translated to $C000–$DDFF

The Gameboy core multiplexes Port B of existing dual-port WRAM and ZPRAM BRAMs between savestate access and RA reads (RA takes priority when `ra_wram_req` / `ra_zpram_req` is asserted). Cart RAM is accessed read-only through SDRAM channel 2 with busy handshaking. A 2-cycle BRAM read path is used (address latch → output valid). The DDRAM arbiter in the core's `ddram.sv` module provides a dedicated channel for RA read/write operations alongside the savestate channel.

#### N64 — Selective Address (`ra_ram_mirror_n64.sv`)
Uses the same selective address protocol but with N64-specific adaptations. The FPGA reads requested byte addresses directly from RDRAM stored in DDR3:
- **RDRAM** ($000000–$7FFFFF) — 4 to 8 MB

Key differences from other cores:
- **Separate DDRAM base address** — N64 savestates occupy 0x3C000000–0x3FFFFFFF, colliding with the default RA mirror base. The N64 mirror is relocated to ARM physical 0x38000000.
- **DDR3 bus arbitration** — A dedicated arbiter (`ddram_arb_n64.sv`) shares the DDR3 bus between the N64 core's 8 DDR3Mux clients (CPU, RSP, RDP, VI, etc.) and the RA mirror. A starvation counter (512-cycle limit, ~4 µs) ensures the RA mirror eventually gets bus access, and a burst mode optimization chains consecutive RA operations without re-entering the starvation wait.
- **Clock domain crossing** — The N64 core's DDR3 controller runs at 125 MHz (`clk_2x`) while the RA mirror runs at ~62.5 MHz (`clk_1x`). Two-stage flip-flop synchronizers handle CDC for the toggle req/ack signals.
- **Direct byte mapping** — RDRAM byte N maps directly to DDRAM byte N within each 64-bit word (the CPU's byteswap32 + memorymux half-swap already places bytes correctly). No additional XOR or byte reordering is needed on the FPGA side.
- **8-VBlank warmup** — The mirror waits ~133 ms after reset before starting, to ensure the VI module (which generates VBlank) has stabilized.

#### PSX — Selective Address (`ra_ram_mirror_psx.sv`)
Same request/response protocol. The FPGA reads requested addresses from SDRAM via a dedicated channel (CH4):
- **Main RAM** ($000000–$1FFFFF) — 2 MB

#### NeoGeo (MVS / AES / CD) — Selective Address (`ra_ram_mirror_neogeo.sv`)
Uses the selective address protocol. The FPGA reads from dual-ported 68K Work RAM BRAMs (WRAML + WRAMU, 8-bit each):
- **68K Work RAM** ($00000–$0FFFF) — 64 KB

The NeoGeo core stores 68K Work RAM in two separate 15-bit DPRAMs — WRAML (low byte, `M68K_DATA[7:0]`) and WRAMU (high byte, `M68K_DATA[15:8]`). The RA mirror reads Port B of both BRAMs using the byte address: even addresses (`addr[0]=0`) select WRAMU and odd addresses (`addr[0]=1`) select WRAML, following the 68K big-endian convention. No address inversion or byte reordering is needed.

In **CD mode**, the WRAML/WRAMU BRAMs are repurposed for Z80 RAM and the 68K Work RAM lives in SDRAM. To keep RA working, shadow DPRAMs capture both CPU writes (via `~nWWL`/`~nWWU`) and DMA writes (targeting `$100000–$10FFFF`). The RA mirror automatically muxes between the MVS BRAMs and the CD shadow BRAMs depending on the active system mode.

A custom DDRAM arbiter (`ddram_arb_neogeo.sv`) sits between the existing DDRAM controller (ADPCM samples / Z80 ROM reads) and the physical DDRAM interface, letting the RA mirror steal bus cycles when the primary master is idle. Two-stage flip-flop synchronizers handle clock domain crossing between CLK_48M (RA mirror) and CLK_96M (DDRAM).

#### TurboGrafx-16 / PC Engine — Selective Address (`ra_ram_mirror_tgfx16.sv`)
Uses the selective address protocol with a dual-path memory architecture. The FPGA reads from BRAM for Work RAM and from DDRAM for CD/SCD RAM:
- **Work RAM** ($000000–$001FFF) — 8 KB (BRAM Port B via `pce_top`, 1-cycle latency)
- **CD RAM** ($002000–$011FFF) — 64 KB (DDRAM at offset 0x0600000)
- **Super CD RAM** ($012000–$041FFF) — 192 KB (DDRAM at offset 0x0610000)

The core's existing `ddram.sv` was extended with an RA arbiter that gives RA secondary priority behind the core. When the core has no pending request and DDRAM is idle, the RA mirror takes over for one transaction. CD/SCD RAM is accessed as 64-bit DDRAM words with byte extraction. The module runs on `clk_sys` (~21.477 MHz) and supports both HuCard-only games (8 KB Work RAM) and CD-ROM/Super CD-ROM titles (up to 264 KB). Total exposed: **264 KB**.

#### Atari 2600 (via Atari7800 core) — Full Mirror (`ra_riot_mirror.sv`)
The simplest mirror in the project — the entire 128-byte RIOT (M6532) internal RAM is copied to DDRAM on every VBlank. No address list or handshake is needed.
- **RIOT RAM** ($0080–$00FF) — 128 bytes (M6532 internal BRAM, async read port)

The Atari7800 core supports both native 7800 games and a 2600 compatibility mode (auto-detected via game header). RA is active **only in 2600 mode** (`tia_en=1`); the mirror writes nothing when a 7800 game is loaded. The M6532 BRAM already had a read port exposed from the emulation; the `ra_riot_mirror.sv` module reads all 128 bytes sequentially via an async port and writes them in 16 × 64-bit chunks to DDRAM channel 2 (a dedicated write-only channel added to the existing `ddram.sv`). Total exposed: **128 bytes**.

#### Sega 32X — Selective Address (`ra_ram_mirror_s32x.sv`)
The most architecturally complex core in the project — the 32X exposes two RAM regions from two entirely different physical backends:
- **68K Work RAM** ($00000–$0FFFF) — 64 KB (on-chip BRAM inside the GEN module, Port B read, 2-cycle latency)
- **SH2 SDRAM / 32X RAM** ($10000–$4FFFF) — 256 KB (DDR3, accessed via the `ddram_arb_s32x` interposer)

Rather than adding a new channel to `ddram.sv`, a dedicated arbiter (`ddram_arb_s32x.sv`) is inserted as an **interposer** between `ddram.sv` and the physical DDR3 pins. When `ddram.sv` is idle (no burst in flight), the arbiter steals one read or write transaction for the RA mirror. Clock domain crossing between `clk_sys` (RA mirror) and `clk_ram` (DDR3 controller) is handled with two-stage flip-flop synchronisers on all toggle signals.

The SH2 is big-endian: byte 0 is at DDR word bits [63:56]. rcheevos expects little-endian byte order within each halfword, so the byte-lane extraction applies `offset ^ 3'd1` to swap lanes correctly. The GEN module was extended with three new ports (`RA_BRAM_ADDR[14:0]`, `RA_BRAM_U[7:0]`, `RA_BRAM_L[7:0]`) exposing a secondary read port on the 68K Work RAM BRAM — no FC1004 address inversion is needed here (unlike the standalone Genesis core). Total exposed: **320 KB**.

### `AddAddress` Support — Pointer Resolution

Several achievement conditions use the `AddAddress` operator, which reads a pointer from memory and adds its value to a base address to obtain the actual memory location to evaluate. In the Selective Address protocol, this creates a bootstrapping problem: on first collection, all cached values are zero, so `AddAddress` conditions compute `base + 0` and only request the pointer address itself — the real target address cannot be known until the pointer's actual value is available.

To solve this, cores that use the Selective Address protocol run a multi-phase pointer-resolution sequence on game load:

1. **Bootstrap** — `rc_client_do_frame()` runs once in *collect mode* with all values assumed zero. This discovers the initial set of addresses, including pointer base addresses.
2. **FPGA response** — The ARM waits for the FPGA to fill the value cache with real data.
3. **Pointer-resolution pass** — `rc_client_do_frame()` runs again in collect mode, now with real pointer values. This resolves derived target addresses that were invisible in the bootstrap pass. For **N64**, this step is repeated up to **4 times** to handle chains of nested `AddAddress` conditions (each pass may reveal addresses that depend on values discovered in the previous pass).
4. **Resume normal processing** — Once the address list stabilises and the final FPGA response arrives, the pipeline enters its normal per-frame polling loop.

**State preservation**: Before and after each bootstrap or resolution pass, the rcheevos client state is serialized (`rc_client_serialize_progress`) and restored afterwards. This prevents zero-value reads during collection from spuriously incrementing hit counters or triggering delta conditions, keeping achievement evaluation correct.

**Periodic re-collection**: During normal gameplay the address list is refreshed to account for pointer changes at runtime (e.g., a game reallocating a data structure):

| Core | Re-collection interval | Re-resolves on change? |
|------|------------------------|------------------------|
| SNES | every ~5 min (~18 000 frames) | No |
| Genesis | every ~5 min (~18 000 frames) | No |
| GBA | every ~5 min (~18 000 frames) | No |
| Mega CD | every ~5 min (~18 000 frames) | No |
| NeoGeo | every ~5 min (~18 000 frames) | No |
| TG16 | every ~5 min (~18 000 frames) | No |
| Atari 2600 | every frame (full mirror, no re-collection needed) | N/A |
| Sega 32X | every ~5 min (~18 000 frames) | No |
| PSX | every ~10 s (~600 frames) | Yes (1 pass) |
| N64 | every ~10 s (~600 frames) | Yes (up to 4 passes) |

### ROM / Disc Hashing

| Core | Method |
|------|--------|
| NES | MD5 of ROM data, skipping 16-byte iNES header and optional 512-byte trainer |
| SNES | MD5 of ROM data, skipping optional 512-byte SMC/SWC copier header (detected when `file_size % 1024 == 512`) |
| Genesis | MD5 of the raw ROM file (no header skipping needed) |
| Mega CD | `rc_hash_generate_from_file()` from rcheevos — handles `.cue+.bin` and `.chd` disc images natively |
| Gameboy / GBC | MD5 of the raw ROM file (no header skipping needed) |
| GBA | `rc_hash_generate_from_file()` from rcheevos — handles `.gba` ROM files natively |
| N64 | `rc_hash_generate_from_file()` from rcheevos — handles `.z64`, `.n64`, and `.v64` byte orders natively |
| PSX | `rc_hash_generate_from_file()` from rcheevos — handles `.cue+.bin`, `.chd`, and `.iso` disc images natively |
| NeoGeo | `rc_hash_generate_from_file()` from rcheevos — handles filename-based hashing for arcade ROM sets natively |
| TG16 (HuCard) | MD5 of ROM data, skipping optional 512-byte copier header (detected when `file_size % 1024 == 512`) |
| TG16 (CD-ROM) | `rc_hash_generate_from_file()` from rcheevos — handles `.cue+.bin`, `.chd`, `.ccd`, `.iso`, and `.img` disc images natively |
| Atari 2600 | MD5 of the raw ROM file (`.a26`) — no header stripping needed |
| Sega 32X | `rc_hash_generate_from_file()` from rcheevos — handles `.32x` ROM files natively |

## How to Try It

1. Download the latest release binaries from the [Releases](https://github.com/odelot/Main_MiSTer/releases) page.
2. Edit `retroachievements.cfg` with your [RetroAchievements](https://retroachievements.org/) credentials:
   ```ini
   username=YourRAUsername
   password=YourRAPassword
   ```
3. Copy **all files** from the release to `/media/fat` on your MiSTer SD card (the `MiSTer` binary, `retroachievements.cfg`, and the `lib/` folder if included).
4. You will also need one or more of the **modified cores**:
   - **NES**: [odelot/NES_MiSTer](https://github.com/odelot/NES_MiSTer)
   - **SNES**: [odelot/SNES_MiSTer](https://github.com/odelot/SNES_MiSTer)
   - **Genesis / Mega Drive**: [odelot/MegaDrive_MiSTer](https://github.com/odelot/MegaDrive_MiSTer)
   - **Master System / Game Gear**: [odelot/SMS_MiSTer](https://github.com/odelot/SMS_MiSTer)
   - **Gameboy / Gameboy Color**: [odelot/Gameboy_MiSTer](https://github.com/odelot/Gameboy_MiSTer)
   - **N64**: [odelot/N64_MiSTer](https://github.com/odelot/N64_MiSTer)
   - **PSX**: [odelot/PSX_MiSTer](https://github.com/odelot/PSX_MiSTer)
   - **GBA**: [odelot/GBA_MiSTer](https://github.com/odelot/GBA_MiSTer)
   - **Mega CD / Sega CD**: [odelot/MegaCD_MiSTer](https://github.com/odelot/MegaCD_MiSTer)
   - **NeoGeo (MVS / AES / CD)**: [odelot/NeoGeo_MiSTer](https://github.com/odelot/NeoGeo_MiSTer)
   - **TurboGrafx-16 / PC Engine**: [odelot/TurboGrafx16_MiSTer](https://github.com/odelot/TurboGrafx16_MiSTer)
   - **Atari 2600** (load a `.a26` ROM in the Atari7800 core): [odelot/Atari7800_MiSTer](https://github.com/odelot/Atari7800_MiSTer)
   - **Sega 32X**: [odelot/S32X_MiSTer](https://github.com/odelot/S32X_MiSTer)
5. Reboot your MiSTer, load the core, and open a game that has achievements on [retroachievements.org](https://retroachievements.org/).
6. (Optional) Place an `achievement.wav` file in `/media/fat/` to hear a sound effect on unlock.

## Building from Source

Follow the standard MiSTer cross-compilation guide [here](https://mister-devel.github.io/MkDocs_MiSTer/developer/mistercompile/#general-prerequisites-for-arm-cross-compiling), then:

```bash
# Clone with submodules (rcheevos)
git clone --recursive https://github.com/odelot/Main_MiSTer.git
cd Main_MiSTer

# If you forgot --recursive:
git submodule update --init --recursive

make
```

The Makefile automatically detects the rcheevos library and enables it if present.

## Links

- Original MiSTer Main binary: [MiSTer-devel/Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer)
- Modified NES core: [odelot/NES_MiSTer](https://github.com/odelot/NES_MiSTer)
- Modified SNES core: [odelot/SNES_MiSTer](https://github.com/odelot/SNES_MiSTer)
- Modified Genesis core: [odelot/MegaDrive_MiSTer](https://github.com/odelot/MegaDrive_MiSTer)
- Modified SMS core: [odelot/SMS_MiSTer](https://github.com/odelot/SMS_MiSTer)
- Modified Gameboy core: [odelot/Gameboy_MiSTer](https://github.com/odelot/Gameboy_MiSTer)
- Modified N64 core: [odelot/N64_MiSTer](https://github.com/odelot/N64_MiSTer)
- Modified PSX core: [odelot/PSX_MiSTer](https://github.com/odelot/PSX_MiSTer)
- Modified GBA core: [odelot/GBA_MiSTer](https://github.com/odelot/GBA_MiSTer)
- Modified MegaCD core: [odelot/MegaCD_MiSTer](https://github.com/odelot/MegaCD_MiSTer)
- Modified NeoGeo core: [odelot/NeoGeo_MiSTer](https://github.com/odelot/NeoGeo_MiSTer)
- Modified TurboGrafx-16 core: [odelot/TurboGrafx16_MiSTer](https://github.com/odelot/TurboGrafx16_MiSTer)
- Modified Atari7800 core (Atari 2600 RA): [odelot/Atari7800_MiSTer](https://github.com/odelot/Atari7800_MiSTer)
- Modified Sega 32X core: [odelot/S32X_MiSTer](https://github.com/odelot/S32X_MiSTer)
- RetroAchievements: [retroachievements.org](https://retroachievements.org/)
- rcheevos library: [RetroAchievements/rcheevos](https://github.com/RetroAchievements/rcheevos)
