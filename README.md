# Main_MiSTer - RetroAchievements Fork

This is a fork of the official [MiSTer Main binary](https://github.com/MiSTer-devel/Main_MiSTer) with **RetroAchievements** support for MiSTer FPGA.

> **Status:** Experimental / Proof of Concept

## Supported Cores

| Core | Console ID | Hardcore | Modified Core Repo |
|------|-----------|----------|--------------------|
| NES | 7 | ✅ Supported | [odelot/NES_MiSTer](https://github.com/odelot/NES_MiSTer) |
| Famicom Disk System (via NES core) | 81 | ✅ Supported | [odelot/NES_MiSTer](https://github.com/odelot/NES_MiSTer) |
| SNES (incl. SA-1 / SuperFX) | 3 | ✅ Supported | [odelot/SNES_MiSTer](https://github.com/odelot/SNES_MiSTer) |
| Genesis / Mega Drive | 1 | ✅ Supported | [odelot/MegaDrive_MiSTer](https://github.com/odelot/MegaDrive_MiSTer) |
| N64 | 2 | ✅ Supported | [odelot/N64_MiSTer](https://github.com/odelot/N64_MiSTer) |
| PSX | 12 | ✅ Supported | [odelot/PSX_MiSTer](https://github.com/odelot/PSX_MiSTer) |
| Gameboy / Gameboy Color | 4 / 6 | ✅ Supported | [odelot/Gameboy_MiSTer](https://github.com/odelot/Gameboy_MiSTer) |
| Master System / Game Gear | 11 / 15 | HC wired, pending validation | [odelot/SMS_MiSTer](https://github.com/odelot/SMS_MiSTer) |
| GBA (Game Boy Advance) | 5 | HC wired, pending validation | [odelot/GBA_MiSTer](https://github.com/odelot/GBA_MiSTer) |
| Mega CD / Sega CD | 9 | HC wired, pending validation | [odelot/MegaCD_MiSTer](https://github.com/odelot/MegaCD_MiSTer) |
| TurboGrafx-16 / PC Engine (incl. CD) | 8 / 76 | HC wired, pending validation | [odelot/TurboGrafx16_MiSTer](https://github.com/odelot/TurboGrafx16_MiSTer) |
| Sega 32X | 10 | HC wired, pending validation | [odelot/S32X_MiSTer](https://github.com/odelot/S32X_MiSTer) |
| Virtual Boy | 28 | HC wired, pending validation | [odelot/VirtualBoy_MiSTer](https://github.com/odelot/VirtualBoy_MiSTer) |
| NeoGeo (MVS / AES / CD) | 27 / 56 | casual only | [odelot/NeoGeo_MiSTer](https://github.com/odelot/NeoGeo_MiSTer) |
| Atari 2600 (via Atari7800 core) | 25 | casual only | [odelot/Atari7800_MiSTer](https://github.com/odelot/Atari7800_MiSTer) |
| Atari 7800 (via Atari7800 core, `.a78`) | 51 | casual only | [odelot/Atari7800_MiSTer](https://github.com/odelot/Atari7800_MiSTer) |
| Sega Saturn | 39 | casual only | [odelot/Saturn_MiSTer](https://github.com/odelot/Saturn_MiSTer) |

See [Hardcore Mode](#hardcore-mode) for what each status means.

## How to Test

Pre-built binaries are available on the [Releases](https://github.com/odelot/Main_MiSTer/releases) page, no compilation needed.

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
| `achievements.cpp / .h` | Core lifecycle - init, load game, per-frame poll, unload, OSD popups, F6 achievement list |
| `achievements_<console>.cpp` | Per-console handlers (NES, SNES, Genesis, MegaCD, SMS, Gameboy, GBA, N64, PSX, NeoGeo, TG16, Atari 2600, Atari 7800, Sega 32X, Saturn) |
| `achievements_console.h` | Console handler interface (`console_handler_t` struct) |
| `achievements_console_lookup.cpp` | Dispatch table mapping core names to console handlers, plus shared Selective Address stall-recovery / resync helpers |
| `ra_http.cpp / .h` | Async HTTP worker thread that executes RetroAchievements API calls via `curl` |
| `ra_ramread.cpp / .h` | Shared DDRAM protocol: full-mirror parsing, the Selective Address list (with the active-snapshot consistency layer), and the RTQuery mailbox client |
| `ra_cdreader_chd.cpp / .h` | Unified CD reader bridge: `.chd` via MiSTer's `libchdr`, `.cue`/`.gdi` via the rcheevos default handler. Registered at startup, enabling CHD disc support for all disc-based consoles (PSX, Mega CD, PCE-CD, NeoGeo CD, Saturn). |
| `shmem.cpp / .h` | Thin wrapper around `/dev/mem` + `mmap` for ARM ↔ FPGA shared memory access |
| `retroachievements.cfg` | User credentials and options  see [Configuration](#configuration) |
| `lib/rcheevos/` | The [rcheevos](https://github.com/RetroAchievements/rcheevos) library, **v12.x** (achievement logic, server protocol, multiset support) |

### Files Modified (hooks into the main loop)

| File | Change |
|------|--------|
| `scheduler.cpp` | Calls `achievements_poll()` every frame |
| `menu.cpp` | Calls `achievements_load_game()` when a ROM is selected; F6 opens the achievement list |
| `main.cpp` | Calls `achievements_init()` / `achievements_deinit()` at startup/shutdown |
| `user_io.cpp` | Notifies the RA layer when a core reset is triggered (keyboard or joystick combo) |
| `Makefile` | Conditionally compiles rcheevos sources and links against them |

No other existing behavior is changed.

## Architecture

The RetroAchievements integration uses a four-layer pipeline that connects the FPGA hardware to the RetroAchievements server:

```
┌───────────────────────────────────────────────────────┐
│  FPGA Core                                            │
│  ra_ram_mirror*.sv exposes emulated RAM to DDRAM      │
│  every VBlank (~60 Hz).                               │
│  N64 and Saturn instead expose RAM directly in DDR3   │
│  and only publish a VBlank heartbeat / frame counter. │
└──────────────────────┬────────────────────────────────┘
                       │  DDRAM (default base: ARM phys 0x3D000000)
                       ▼
┌───────────────────────────────────────────────────────┐
│  ARM Binary (this repo)                               │
│  shmem.cpp  - mmap /dev/mem to read DDRAM             │
│  ra_ramread - protocol parsing, Selective Address     │
│               list + active snapshot, RTQuery mailbox │
└──────────────────────┬────────────────────────────────┘
                       │
                       ▼
┌───────────────────────────────────────────────────────┐
│  rcheevos SDK (v12, multiset-aware)                   │
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

1. **Init** - On startup, the ARM binary maps the DDRAM mirror region, starts an HTTP worker thread, and loads credentials from `retroachievements.cfg`. The hardcore FPGA bits are applied (or explicitly cleared) at core init so stale restrictions never leak between sessions.
2. **Load Game** - When a ROM is selected in the MiSTer menu, the binary checks internet connectivity (DNS + non-blocking TCP probe to `retroachievements.org`). If online and not yet logged in, it logs in to RetroAchievements on the spot, then computes the ROM's hash and identifies the game against the RA database. `.a78` files automatically switch the Atari7800 core's handler from Atari 2600 to Atari 7800, and `.fds` files switch NES to Famicom Disk System. If no internet is detected, an OSD message is shown and login is deferred. Loading the **standard community core** (which lacks the RA mirror module) silently suppresses all RA activity - no spurious login or network calls are made.
3. **Per-Frame Poll** - Every frame (~60 Hz), `achievements_poll()` checks whether the FPGA has produced a new frame of data. If so, it calls `rc_client_do_frame()` from the rcheevos library, which evaluates achievement conditions against the current RAM state.
4. **Unlock / Notify** - When an achievement triggers, the event handler displays an OSD notification and optionally plays a sound (`/media/fat/achievement.wav`). The unlock is reported to the RA server asynchronously. Leaderboard events (start, fail, submit, scoreboard result) also show OSD notifications; a tracker display updates in real time while a leaderboard is active.
5. **Core Reset** - When the user triggers a core reset (keyboard shortcut, joystick combo, or OSD), the RA layer is notified so the rcheevos client can reset its internal state correctly. The FPGA frame counter going backwards (e.g. after a savestate load or PAL/NTSC switch) is also detected and resynced automatically.
6. **Unload / Shutdown** - State is cleaned up when switching cores or shutting down.

### Achievement List (F6)

While the OSD is open and a game with achievements is loaded, press **F6** to open a scrollable achievement list for the current game. The list shows all core achievements grouped with **unlocked entries first** (marked `►`) followed by **locked entries** (marked `◄`). The OSD title displays the total count, e.g. *Achievements (42)*.

For **multiset games** (rcheevos 12+: a game with subsets, e.g. bonus or challenge sets), the list shows **one page per set**: the footer displays the current set's name with Left/Right arrows, and **← / →** switch between sets.

Navigation inside the list:

| Key | Action |
|-----|--------|
| ↑ / ↓ | Move one entry |
| Page Up / Page Down | Previous / next page |
| ← / → | Previous / next set (multiset games) |
| Home / End | Jump to first / last entry |
| Menu button | Close and return to normal OSD |

## Hardcore Mode

Achievements run in **casual mode** by default. Set `hardcore=1` in `retroachievements.cfg` to request hardcore mode.

Hardcore is only actually engaged on cores that enforce the restrictions **in hardware** (`hardcore_protected` in the console handler). On those cores, `set_hardcore()` writes FPGA status bits that disable cheats and block savestate restore at the core level.

| Status | Cores | Meaning |
|--------|-------|---------|
| ✅ **Officially Supported** | NES, FDS, SNES, Genesis/MD, N64, PSX, GB/GBC | FPGA guardrails validated; `hardcore=1` engages real hardcore (cheats disabled, restore-state blocked in hardware) |
| 🔧 **Wired, in validation** | SMS/GG, GBA, MegaCD, TG16, 32X | The core RTL has the hardcore guardrails and Main maps the status bits, but the homologation checklist isn't complete - these cores still run casual unless you set `force_hardcore=1` (for testing at your own risk) |
| - **casual only** | NeoGeo, Atari 2600/7800, Saturn | No hardcore bits wired (2600/7800/Saturn cores have no cheat engine / savestates to block; NeoGeo pending) |

Per-core enforcement details (status bits written by Main):

- **NES/FDS** - hardcore master bit blocks *restore*-state and disables cheats; creating save states remains allowed (they just can't be loaded back in hardcore).
- **SNES** - cheats disabled + save states disabled.
- **Genesis / Mega Drive** - cheats disabled + save states disabled.
- **N64** / **PSX** - dedicated hardcore signal + cheats OSD toggle forced off.
- **SMS/GG, GB/GBC, GBA, MegaCD, TG16, 32X** - equivalent bits are wired (hardcore signal + cheats off; GBA additionally gates underclock, rewind and fast-forward) pending hardware validation.

`force_hardcore=1` bypasses the per-core gate and requests hardcore everywhere - useful for validating a core, not recommended for regular play.

## RAM Exposure Protocols

There are three protocols for exposing emulated RAM to the ARM:

- **Full Mirror** - The FPGA copies all relevant RAM to DDRAM every VBlank. Simple, but only viable for small RAM spaces (NES fallback, Atari 2600/7800).
- **Selective Address** - The ARM writes the list of addresses rcheevos needs; the FPGA reads only those values each VBlank and writes them back to a value cache. Used by most cores. FPGA **v2** mirrors additionally implement an **RTQuery mailbox** (a small request/response area polled between VBlanks) that lets the ARM read arbitrary bytes on demand. (Source code and logs still refer to this protocol by its historical working name, "Option C".)
- **Direct DDR3 mapping** - For cores whose work RAM already lives in DDR3 (N64, Saturn), the ARM simply `mmap`s the RAM region and reads bytes directly; the FPGA only publishes a frame counter / VBlank heartbeat to pace evaluation.

### Selective Address operating modes

- **Smart Cache (default for every Selective Address core when the FPGA is v2, except SMS)** - one bootstrap collection seeds the FPGA cache; from then on, cache misses (typically `AddAddress` pointer targets that moved) are answered **live** through the RTQuery mailbox and appended to the FPGA list incrementally. There is no periodic re-collection; a cleanup pass (~1/min, growth-gated) prunes stale dynamic addresses, which simply re-add themselves via misses if still needed. `smart_cache=0` can still be set to force Legacy mode on any core (e.g. for debugging).
- **Legacy** - for v1 FPGAs (no mailbox): periodic re-collection (default every ~5 min, `recollect_interval`) refreshes the address list; misses read as 0 until the next recollect.

### Consistency layer - the active snapshot

The Selective Address value cache is *positional*: the FPGA writes values in the order of the address list it last read. Naively indexing it with the ARM's live list would return the *neighbour's* byte whenever the list changes (a dynamic insert shifts every index after it) - enough to fire spurious achievements. The shared `ra_ramread` layer prevents this by construction:

- Three list copies are kept: the **pending** list (mutated live by dynamic adds), the **published** list (exactly what was last written to DDRAM with the current `request_id`), and the **active snapshot** (the last revision the FPGA *confirmed* via `response_id`). All cached reads go through the active snapshot, so static addresses never misalign and never lose the cache, across every insert / prune / recollect.
- **Revisions are serialized**: a new list is only published after the FPGA confirmed the previous one, so at most one revision is ever in flight. Until the FPGA confirms, addresses not yet in the active snapshot are simply served by RTQuery (Smart Cache) or read as 0 with achievement processing gated (legacy).
- `request_id` is **monotonic for the process lifetime**, so a posthumous FPGA response written across a core reset can never be mistaken for a fresh one.

### Anti-false-positive guards (all Selective Address cores)

- **Bootstrap re-prime** - the bootstrap collection frame runs with all reads returning zero. `rc_client_reset()` is called *before* it (so a mid-game re-bootstrap can't fire active triggers on zeros) and again when the cache becomes active (so delta conditions can't see spurious 0 → real transitions). Cores with multi-pass pointer resolution (PSX, NeoGeo) only re-prime after zero-read bootstraps, preserving hit counts across real-value re-collections.
- **Backward-frame resync** - a frame counter that goes backwards (core reset, savestate load) resyncs tracking instead of stalling it.

### DDRAM Mirror Layout

Every Selective Address / Full Mirror core writes a structured block at the RA base address (default ARM physical `0x3D000000`; N64 uses `0x38000000`, Saturn `0x30F00000`):

| Offset | Content |
|--------|---------|
| `0x00000` | Header: magic `"RACH"`, region count, flags (busy bit), FPGA core version, frame counter |
| `0x00040` | ARM-written config bits (e.g. RTQuery arming, GBA clear enable) |
| `0x00100+` | RAM data area (layout varies per core - Full Mirror protocol) |
| `0x40000` | Address request list - ARM writes `{addr_count, request_id}` + addresses |
| `0x48000` | Value response cache - FPGA writes `{response_id, response_frame}` + values |
| `0x50000` | RTQuery mailbox (v2 FPGAs): control word + 16 request/response slots |

## Per-Core Details

#### NES / Famicom Disk System - Smart Cache + Realtime Query (`ra_ram_mirror_nes.sv`)
Current FPGA is v2: selective address reading with RTQuery mailbox, Smart Cache **on by default**. Older v1 cores fall back to the original full mirror (`ra_ram_mirror.sv`, ~10 KB per VBlank), which the ARM auto-detects via the header's region count.
- **CPU-RAM** ($0000–$07FF) - 2 KB
- **Cart SRAM** ($6000–$7FFF) - 8 KB

When a `.fds` file is loaded, the ARM side automatically switches from NES console ID 7 to **Famicom Disk System ID 81** for correct game identification. FDS hashing skips the 16-byte fwNES header (`FDS\x1A`) before MD5.

#### SNES - Smart Cache + Realtime Query (`ra_ram_mirror_snes.sv`)
Selective address reading with RTQuery mailbox; Smart Cache on by default. The scan is triggered **pre-VBlank (~line 215)** so WRAM is captured before the NMI handler runs, matching the timing RA sets are authored against (RASnes9x):
- **WRAM** ($000000–$01FFFF) - 128 KB (read live from SDRAM via the SNI-shared port)
- **BSRAM** ($020000+) - up to 256 KB (includes SA-1 BWRAM, so SA-1 games like Super Mario RPG work; BSRAM reads are VBlank-gated to stay frame-coherent with WRAM)

The core also thrashes WRAM/VRAM/ARAM/BSRAM with init patterns on every cart load, so a previous game's RAM can never leak into a new session.

#### Genesis / Mega Drive - Smart Cache + Realtime Query (`ra_ram_mirror_md.sv`)
Selective address reading with RTQuery mailbox; Smart Cache on by default. The FPGA reads requested addresses directly from the 68K Work RAM BRAM:
- **68K Work RAM** ($000000–$00FFFF) - 64 KB
- Includes hardware-accurate FC1004 address bit-13 inversion for correct BRAM mapping

#### GBA (Game Boy Advance) - Smart Cache + Realtime Query (`ra_ram_mirror_gba.sv`)
Selective address reading with RTQuery mailbox; Smart Cache on by default - required here because Pokémon R/S/E/FR/LG relocate their SaveBlocks in EWRAM at runtime (anti-cheat DMA), so pointer targets change constantly. The FPGA reads from three distinct memory backends:
- **IWRAM** ($00000–$07FFF) - 32 KB (on-chip block RAM, Port B read with double-read collision detection)
- **EWRAM** ($08000–$47FFF) - 256 KB (DDR3 or SDRAM depending on core storage mode)
- **Cart RAM / Flash** ($48000–$57FFF) - up to 64 KB

The GBA core's `ddram.sv` was extended with a dedicated RA channel that never disturbs the ROM prefetch cache, and `sdram.sv` gained an RA read/write channel for SDRAM storage mode. On game load (with `gba_reset_ram=1`) the FPGA clears IWRAM/EWRAM and fills the flash region with `0xFF` (erased state) in both storage modes, before the game starts. Out-of-range pointer targets (garbage mid-update) are answered with 0 without polluting the dynamic list. Total exposed: **352 KB**.

#### Mega CD / Sega CD - Smart Cache + Realtime Query (`ra_ram_mirror_mcd.sv`)
Selective address reading with RTQuery mailbox; Smart Cache on by default. The FPGA reads from SDRAM (not BRAM), handling two distinct memory regions:
- **68K Work RAM** ($00000–$0FFFF) - 64 KB (SDRAM bank 1)
- **MCD Program RAM** ($10000–$8FFFF) - 512 KB (SDRAM banks 0/1 with bank switching)

Unlike the MegaDrive core, no DDRAM arbitration is needed (`ddram_ra_mcd.sv` is a simple pass-through) because the Mega CD core has no other DDRAM consumer. Total exposed: **576 KB**.

#### Master System / Game Gear - Selective Address (`ra_ram_mirror_sms.sv`)
Selective address protocol (legacy mode). The FPGA reads from dual-ported System RAM and NVRAM:
- **System RAM** ($0000–$1FFF) - 8 KB (Z80 $C000–$DFFF mirrored)
- **NVRAM / Cart RAM** ($2000–$9FFF) - up to 32 KB

The SMS core converts its existing single-port RAMs to dual-port (`dpram`) so the RA mirror can read on Port B without disturbing the CPU on Port A. A custom DDRAM arbiter (`ddram_arb_sms.sv`) shares bus access between the framebuffer and the RA mirror. Game Gear titles (console ID 15) are handled by the same handler.

#### Gameboy / Gameboy Color - Smart Cache + Realtime Query (`ra_ram_mirror_gb.sv`)
Selective address reading with RTQuery mailbox; Smart Cache on by default. The FPGA reads from dual-ported WRAM and ZPRAM (HRAM), plus cart RAM via a dedicated SDRAM channel:
- **WRAM** ($C000–$DFFF + GBC banks 2–7 via $10000–$15FFF) - up to 32 KB
- **Cart RAM** ($A000–$BFFF + banks 1–15 via $16000–$33FFF) - up to 128 KB via SDRAM ch2
- **HRAM / ZPRAM** ($FF80–$FFFE) - 127 bytes
- **Echo RAM** ($E000–$FDFF) - automatically translated to $C000–$DDFF

The Gameboy core multiplexes Port B of existing dual-port WRAM and ZPRAM BRAMs between savestate access and RA reads (RA takes priority when requested). Cart RAM is accessed read-only through SDRAM channel 2 with busy handshaking.

#### N64 - Direct RDRAM Mapping + VBlank Heartbeat (`ra_ram_mirror_n64.sv`)
N64 uses a hybrid model closer to emulator-style memory access than Selective Address. The ARM maps RDRAM directly and reads bytes on demand; the FPGA mirror only provides a reliable frame heartbeat.
- **RDRAM** ($000000–$7FFFFF) - 4 to 8 MB (ARM direct mmap at physical 0x30000000, `addr ^ 3` byte-order correction)

Key points:
- **VBlank heartbeat only** - `ra_ram_mirror_n64.sv` writes a compact header at ARM physical 0x38000000 (`RACH` magic, frame counter, version) and increments on each VI interrupt (more stable than video VBlank during transitions).
- **Optional snapshot mode** - with `n64_snapshot=1`, Main copies 8 MB of RDRAM at each VBlank and evaluates achievements against this consistent snapshot instead of live RAM.
- The RA base stays at 0x38000000 to avoid the N64 savestate slots at 0x3C000000–0x3FFFFFFF.

#### PSX - Smart Cache + Realtime Query + flat mirror (`ra_ram_mirror_psx.sv`)
PSX uses selective-address transport with Smart Cache + RTQuery (default on), plus an ARM-side optimization: a **flat 2 MB mirror keyed by address** (O(1) lookup instead of binary search) that is refreshed from the FPGA value cache once per VBlank, but only when the FPGA has confirmed the current list revision, so ordering changes can never corrupt it.
- **Main RAM** ($000000–$1FFFFF) - 2 MB

Cache misses are resolved live through the RTQuery mailbox; a growth-gated cleanup (~every 10 s, only if the list grew >50% over the bootstrap baseline) re-collects and prunes stale entries. Newly collected addresses stay unmonitored until they get a real value, self-healing via RTQuery misses.

#### NeoGeo (MVS / AES / CD) - Smart Cache + Realtime Query (`ra_ram_mirror_neogeo.sv`)
Selective address reading with RTQuery mailbox; Smart Cache on by default, with multi-pass pointer resolution at load. The FPGA reads from dual-ported 68K Work RAM BRAMs (WRAML + WRAMU, 8-bit each):
- **68K Work RAM** ($00000–$0FFFF) - 64 KB

Even addresses select WRAMU and odd addresses WRAML, following the 68K big-endian convention. In **CD mode** (console ID 56), the WRAML/WRAMU BRAMs are repurposed for Z80 RAM and the 68K Work RAM lives in SDRAM; shadow DPRAMs capture both CPU and DMA writes so RA keeps working, muxed automatically by system mode. A custom DDRAM arbiter (`ddram_arb_neogeo.sv`) lets the RA mirror steal bus cycles when the ADPCM/Z80-ROM master is idle, with two-stage synchronizers between CLK_48M and CLK_96M.

#### TurboGrafx-16 / PC Engine - Smart Cache + Realtime Query (`ra_ram_mirror_tgfx16.sv`)
Selective address reading with RTQuery mailbox; Smart Cache on by default, with a dual-path memory architecture:
- **Work RAM** ($000000–$001FFF) - 8 KB (BRAM Port B via `pce_top`)
- **CD RAM** ($002000–$011FFF) - 64 KB (DDRAM)
- **Super CD RAM** ($012000–$041FFF) - 192 KB (DDRAM)

The core's `ddram.sv` was extended with an RA arbiter that gives RA secondary priority behind the core. Supports both HuCard-only games (8 KB) and CD-ROM/Super CD-ROM titles (up to 264 KB; PCE-CD games use console ID 76). Total exposed: **264 KB**.

#### Atari 2600 (via Atari7800 core) - Full Mirror (`ra_riot_mirror.sv`)
The simplest mirror in the project - the entire 128-byte RIOT (M6532) internal RAM is copied to DDRAM on every VBlank. No address list or handshake is needed.
- **RIOT RAM** ($0080–$00FF) - 128 bytes

Active only in 2600 compatibility mode (`tia_en=1`).

#### Atari 7800 (via Atari7800 core) - Full Mirror (`ra_7800_mirror.sv`)
When a `.a78` ROM is loaded, Main switches to the Atari 7800 handler (console ID 51) and the FPGA mirrors the 7800's two 2 KB RAM banks every VBlank as two header-described regions:
- **RAM0** (CPU $2000–$27FF, with $0040–$00FF / $0140–$01FF zero-page and stack mirrors) - 2 KB
- **RAM1** (CPU $1800–$1FFF) - 2 KB

Hashing strips the 128-byte `.a78` header (identified by the `ATARI7800` magic) before MD5, matching `rc_hash_7800()`.

#### Sega 32X - Smart Cache + Realtime Query (`ra_ram_mirror_s32x.sv`)
Selective address reading with RTQuery mailbox; Smart Cache on by default. The 32X exposes two RAM regions from two entirely different physical backends:
- **68K Work RAM** ($00000–$0FFFF) - 64 KB (on-chip BRAM inside the GEN module, Port B read)
- **SH2 SDRAM / 32X RAM** ($10000–$4FFFF) - 256 KB (DDR3, via the `ddram_arb_s32x` interposer)

A dedicated arbiter (`ddram_arb_s32x.sv`) is inserted as an **interposer** between `ddram.sv` and the physical DDR3 pins, stealing one transaction when the core is idle, with two-stage synchronizers between `clk_sys` and `clk_ram`. The SH2 is big-endian; byte-lane extraction applies `offset ^ 1` so rcheevos sees the expected byte order. Total exposed: **320 KB**.

#### Virtual Boy - Smart Cache + Realtime Query (`ra_ram_mirror_vb.sv`)
Selective address reading with RTQuery mailbox; Smart Cache on by default. The two regions come from different backends:
- **System RAM / WRAM** ($00000–$0FFFF) - 64 KB (BRAM **port B dedicated to RA**, two byte lanes: upper = odd CPU addresses, lower = even)
- **Cartridge RAM** ($10000–$1FFFF) - 64 KB window, served from a **coherent DDR3 shadow** at 0x30000000

Cart RAM isn't read from the live SRAM: every CPU/P1 write commits to the DDR3 shadow before completing, so the shadow is always current and RA reads never contend with the game. Byte mapping follows what the CPU sees at `0x06000000 + n`, using the active address mask and packed flag supplied by the emu level - `(n & mask) >> 1` for the packed x8 layout (exact 32 KiB saves, HyperFlash32), `(n & mask)` for the container layout. Addresses outside both regions return `0x00`. The mirror runs on `clk_sys` (40 MHz) and shares one DDRAM channel for both the RA protocol traffic and the shadow reads.

#### Sega Saturn - Direct DDR3 Mapping + frame counter
Like N64, no per-address FPGA transport at all - the Saturn core's work RAM already lives in DDR3, so the ARM maps it directly:
- **Work RAM Low** ($000000–$0FFFFF) - 1 MB (physical 0x30000000)
- **Work RAM High** ($100000–$1FFFFF) - 1 MB (physical 0x30300000)

The FPGA-side RA logic is a single-word writer: it only updates a frame counter in a 12 MB DDR3 gap (0x30F00000); the static `RACH` header is written once by the ARM at init. Byte-order note: the core's DDR3 stores SH-2 byte K at offset `K^7` while RA sets are authored against beetle-saturn (`K^1`), so reads apply `addr ^ 6`.

### `AddAddress` Support - Pointer Resolution

Several achievement conditions use pointer operators (`AddAddress`/`AddSource`) that derive effective addresses at runtime. In Selective Address pipelines this creates a bootstrapping problem: on first collection, cached values are zero, so pointer expressions initially resolve to incomplete targets.

- **Smart Cache mode (default on NES / SNES / Genesis / PSX / GBA)** - pointer targets are discovered *live*: a miss is answered immediately by RTQuery and the address joins the FPGA batch from the next VBlank. No pointer re-collection loop exists at all.
- **Legacy mode** - the bootstrap pass discovers base addresses; the FPGA fills real values; a pointer-resolution pass (PSX/NeoGeo run up to 4) re-collects with real pointer values; then periodic re-collection (default ~5 min, `recollect_interval`) tracks pointers that move later. Achievement processing is paused for the 1–2 frames while a new list revision is in flight.
- **N64 / Saturn (direct mapping)** - pointer operators are evaluated directly against mapped RAM every frame; nothing to resolve.

In all modes, the bootstrap's zero-value frame is neutralized with `rc_client_reset()` (see [Anti-false-positive guards](#anti-false-positive-guards-all-option-c-cores)) so it can neither fire triggers nor poison delta conditions.

### ROM / Disc Hashing

| Core | Method |
|------|--------|
| NES | MD5 of ROM data, skipping 16-byte iNES header and optional 512-byte trainer |
| Famicom Disk System (FDS) | MD5 of disk data, skipping 16-byte fwNES FDS header (`FDS\x1A`) |
| SNES | MD5 of ROM data, skipping optional 512-byte SMC/SWC copier header (detected when `file_size % 1024 == 512`) |
| Genesis | MD5 of the raw ROM file (no header skipping needed) |
| Master System / Game Gear | MD5 of the raw ROM file |
| Gameboy / GBC | MD5 of the raw ROM file |
| Mega CD | `rc_hash_generate_from_file()` from rcheevos - handles `.cue+.bin` and `.chd` disc images natively |
| GBA | `rc_hash_generate_from_file()` from rcheevos - handles `.gba` ROM files natively |
| N64 | `rc_hash_generate_from_file()` from rcheevos - handles `.z64`, `.n64`, and `.v64` byte orders natively |
| PSX | `rc_hash_generate_from_file()` from rcheevos - handles `.cue+.bin`, `.chd`, and `.iso` disc images natively |
| NeoGeo | `rc_hash_generate_from_file()` from rcheevos - filename-based hashing for arcade ROM sets |
| TG16 (HuCard) | MD5 of ROM data, skipping optional 512-byte copier header |
| TG16 (CD-ROM) | `rc_hash_generate_from_file()` from rcheevos - handles `.cue+.bin`, `.chd`, `.ccd`, `.iso`, and `.img` disc images natively |
| Atari 2600 | MD5 of the raw ROM file (`.a26`) |
| Atari 7800 | MD5 of ROM data, skipping the 128-byte `.a78` header when present (matches `rc_hash_7800`) |
| Sega 32X | `rc_hash_generate_from_file()` from rcheevos - handles `.32x` ROM files natively |
| Saturn | `rc_hash_generate_from_file()` from rcheevos - handles `.cue+.bin` and `.chd` disc images natively |

## Configuration

All options live in `/media/fat/retroachievements.cfg`:

| Key | Default | Description |
|-----|---------|-------------|
| `username` / `password` | - | RetroAchievements credentials |
| `hardcore` | 0 | Request hardcore mode on cores with validated hardware enforcement |
| `force_hardcore` | 0 | Force hardcore even on unvalidated cores (testing only) |
| `show_challenge_show_popup` | 1 | Popup when a challenge indicator appears |
| `show_challenge_hide_popup` | 0 | Popup when a challenge indicator disappears |
| `show_progress_popups` | 1 | Popups for achievement progress updates |
| `show_progress_name` | 1 | Include achievement name in progress popups |
| `show_leaderboards_updates` | 1 | Leaderboard start/fail/tracker popups |
| `show_leaderboards_submission` | 1 | Leaderboard submit/scoreboard popups |
| `leaderboards_enabled` | 1 | Enable leaderboard processing |
| `multiline_desc` | 0 | Two-line achievement text in OSD popups |
| `popup_position` | left | Popup corner on the top of the screen: `left`, `center` or `right`. Use `center` when the display crops the sides (e.g. HDMI forced to 4:3 on a TV) and the popup gets cut off. Unsupported values fall back to `left` |
| `smart_cache` | auto | Force Smart Cache on (`1`) / off (`0`); default: on for every Selective Address core except SMS (v2 FPGAs) |
| `rtquery` | 1 | Enable the RTQuery mailbox (v2 FPGAs) |
| `smart_cleanup` | 1 | Periodic pruning of stale dynamic addresses |
| `recollect_interval` | 600 | Legacy-mode re-collection interval, in frames (PSX/GBA legacy; SNES-style cores use ~18000) |
| `stall_recovery` | 0 | Re-bootstrap automatically if the FPGA stops responding ≥5 s |
| `n64_snapshot` | 0 | N64: evaluate against an 8 MB RDRAM snapshot per VBlank |
| `gba_reset_ram` | 1 | GBA: clear IWRAM/EWRAM and fill flash with `0xFF` before each game load |
| `watch` | - | Debug: comma-separated RA addresses to log on change |
| `debug` | 0 | Verbose RA logging |

## How to Try It

1. Download the latest release binaries from the [Releases](https://github.com/odelot/Main_MiSTer/releases) page.
2. Edit `retroachievements.cfg` with your [RetroAchievements](https://retroachievements.org/) credentials:
   ```ini
   username=YourRAUsername
   password=YourRAPassword
   ```
3. Copy **all files** from the release to `/media/fat` on your MiSTer SD card (the `MiSTer` binary, `retroachievements.cfg`, and the `lib/` folder if included).
4. You will also need one or more of the **modified cores**:
   - **NES / FDS**: [odelot/NES_MiSTer](https://github.com/odelot/NES_MiSTer)
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
   - **Atari 2600 / 7800** (load a `.a26` or `.a78` ROM in the Atari7800 core): [odelot/Atari7800_MiSTer](https://github.com/odelot/Atari7800_MiSTer)
   - **Sega 32X**: [odelot/S32X_MiSTer](https://github.com/odelot/S32X_MiSTer)
   - **Sega Saturn**: [odelot/Saturn_MiSTer](https://github.com/odelot/Saturn_MiSTer)
   - **Virtual Boy**: [odelot/VirtualBoy_MiSTer](https://github.com/odelot/VirtualBoy_MiSTer)
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
- Modified Atari7800 core (Atari 2600 + 7800 RA): [odelot/Atari7800_MiSTer](https://github.com/odelot/Atari7800_MiSTer)
- Modified Sega 32X core: [odelot/S32X_MiSTer](https://github.com/odelot/S32X_MiSTer)
- Modified Saturn core: [odelot/Saturn_MiSTer](https://github.com/odelot/Saturn_MiSTer)
- Modified Virtual Boy core: [odelot/VirtualBoy_MiSTer](https://github.com/odelot/VirtualBoy_MiSTer)
- RetroAchievements: [retroachievements.org](https://retroachievements.org/)
- rcheevos library: [RetroAchievements/rcheevos](https://github.com/RetroAchievements/rcheevos)
