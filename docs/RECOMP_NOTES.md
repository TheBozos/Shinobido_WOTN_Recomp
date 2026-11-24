# Recompilation notes: Shinobido: Way of the Ninja (PAL, SCES-53931)

How the port is brought up, every fix and how it was found, and what remains open. Built on
[BozoEngine](https://github.com/TheBozos/BozoEngine) (submodule `BozoEngine/`, pinned at `fb8eff2`, master's head on
2026-11-23), modelled on The Third Age's layout (a stripped ELF named by the symbol matcher) and Onimusha's bootstrap.

## The disc (SCES-53931, version 1.00)

- `SYSTEM.CNF`: `BOOT2 = cdrom0:\SCES_539.31;1`, `VER = 1.00`, `VMODE = PAL`. `SCES_539.31` SHA-1
  `dbadec418726d0049c4311ef6fa97204cf6fe3f8`.
- **The ELF is stripped**: two program headers, one LOAD `0x100000` (file `0x525a00` bytes, memory `0x57c280`: BSS
  up to `0x67c280`) and an empty one; no symbols (`.symtab` is empty), sections unnamed. Entry `0x100008`.
  `.comment` and `.reginfo` only. Code and data share the one segment.
- **SDK 3.0** (2004-2005): `PsIIlibkernl 3000`, `libgraph 3000`, `libcdvd 3000`, `libdma 3000`, `libipu 3000`,
  `libvu0 3000`, `libscf 3000`, `libsdr 3000`, `libpad 3020`, `libmc 3030`. Newer than every reference executable
  that keeps its symbols (DC, TMB, Onimusha: 2.0-2.1), so more of the SDK needs naming by hand than TTA's (2.8).
- **IOP modules**, paths in the ELF at `0x504e00` (`iop/modules/*.irx`) and `0x5051e0` (`ioprp300.img`). On the disc
  (`IOP/MODULES`): `IOPRP300.IMG`, `SIO2MAN`, `PADMAN`, `MCMAN`, `MCSERV`, `LIBSD`, `SDRDRV`, `CDVDSTM`, `USBD`,
  `USBPSPCM`, and Acquire's sound driver `TORAST.IRX` / `TORASE.IRX` (CodeWarrior 2.4.1.01 IOP modules, "Ads_driver";
  `ToraSt: Un Known Command %x`). The ELF also names `rtfssiop`, `modmidi`, `modhsyn`, `gzmfs`, `rwa`, `rtfshost`,
  `rtfsdvd` and the network stack (`ent_*`, `netcnf`, `eenetctl`, `dev9`), not on the disc: development or network
  paths, to be checked when the loads are reached.
- **Data**: `VOLUME.DAT` (547 MB, opened as `volume.dat` at `0x505238`; `chara/*.bin`, `weapon/wpdata.bin`,
  `objparam.bin`, `pact/pactfile.bin`, `vib/vibdata.bin` are paths inside it), `ACQ.BIN` (358 MB). Sound: `SOUND/BGM.TRS`,
  `STPACK.TRS`, `VLETTER.TRS`, `CUTIN.TRS` (paths at `0x504a00`; "TrsFile Version Error"). Movies: PSS, `PAL/COMMON`
  (`ACQUIRE.PSS`, `NINJA.PSS`, `STAFF.PSS`) and per voice language `PAL/MVE` (English) / `PAL/MVJ` (Japanese):
  story films `K*`, `S*`, the letters `VL/VL*`, the tutorials `TUTORIAL/TU*`. libipu / libmpeg are linked
  (`sceMpegGetPicture`).

## Build

- `scripts/symbols.sh <reference ELFs>` rebuilds `recomp/shino_symbols.txt` with BozoEngine's
  `tools/symbolmap/bozo_symbolmap.py`. First map (DC's `SCUS_971.11`, TMB's `SCUS_971.01`, Onimusha's `SLUS_200.18`):
  **306 functions** (exact 226, layout 60, calls 18, drift 2); of the 271 names the engine answers natively, 122.
  With the reviewed names: 326, 142 of the 271.
  Named: the kernel calls, much of libgraph (`sceGsSyncV` `0x2c2bb0`, `sceGsSwapDBuffDc` `0x2c3998`, `sceGsResetPath`
  `0x2c1ec8`, `sceGsSyncPath` `0x2c2c48`), libdma, libvu0, `sceSifResetIop` `0x2cef40`. Not yet: `sceSifInitRpc`,
  `sceSifLoadModule` (two same-shape candidates, `0x2cec00`), the IOP reset (`sceSifRebootIop`, `sceSifSyncIop`),
  libpad (`scePadInit`, `scePadPortOpen`; `scePadRead` candidate `0x2d40a0`), the fileio client (`sceOpen`..),
  libmpeg. TTA's way: read from the code into `recomp/shino_symbols_reviewed.txt`, pairing each `sceSifCallRpc`
  caller with its server id and function number.
- `tools/shino_overrides_list.py` lists the `registerFunction(0x...)` overrides and `case 0x...u: // native site`
  lines of `runtime/`; `bozo-externals` adds the library and kernel stubs from the symbol map (159 stubs).
- The runner passes the symbol map to the host (`FreeOptions::symbolMap`, `--symbols FILE`): the host binds the
  stubs to the engine's native functions by name; without it every stub is unknown.
- `tools/shino_rpc_names.py` (TTA's `tta_rpc_names.py`) pairs every `sceSifCallRpc` caller with its server id and
  function number in the target and the references: it named libmc's clients (server `0x80000400`) and libcdvd's
  `sceCdInit` (`0x80000592`) and `sceCdSearchFile` (`0x80000597`) in one pass. libpad's clients (`0x80000100`, all
  function 1) need reading. `tools/shino_disasm.py` disassembles with the map's names (capstone, in the WindowsApps
  Python: `py tools/shino_disasm.py game/SCES_539.31 0xADDR [bytes]`).
- `tools/shino_prepare.py` writes `build/bozo/shino_prep/shino.elf`, the image bozo-recomp translates and the runner
  loads (program headers unchanged; the SHA-1 is checked). The ELF's one section covers code, VU microcode and data
  alike: translated as is, ~1 MB of data became "functions" (one 13.8 MB generated file). The section is cut to the EE
  code, `0x100000-0x4f1500` (the VU microcode starts at `0x4f1500`, right after the last `jr ra`), and a second code
  section is added for the **C++ static constructors** the linker left among the data, `0x614b80-0x617484`: their
  table `0x617490-0x6174e4` is walked by `0x101340` (called from `0x100c30`, from main `0x100ba0`), and each
  constructor registers its object's destructor through `0x101310`. No `jal` from the EE code reaches past
  `0x4f1500`; the other R5900 code in the data, `0x524d18..0x526238`, reads kernel memory (`0x80075300`): the SDK's
  kernel patch, which the engine's native `_InitSys` replaces. The data after the constructors (`0x6174f0`: address
  ranges `0x101000`, `0x101050`, ...) is CodeWarrior's exception table: the game is a CodeWarrior build, like Dark
  Cloud and Onimusha.
- `bozo-recomp` translates `shino.elf` into 32 files (`build/bozo/shino_gen/`, prefix `shino`); the lists live in
  `build/bozo/shino_prep/` (bozo-recomp removes any other `shino_*` file in its output folder).
- `scripts/build_runtime.sh` (cmake + ninja), `scripts/run.sh --frames N [--profile-io]`.

## Boot, step by step

Each was a stop of the headless runner (`scripts/run.sh --frames 300`), 2026-11-23.

1. `syscall 116 at 0x2cfd4c`: SetSyscall, from the SDK's kernel patch `0x2cfc48`, called by `InitSys` `0x2cfd58`
   (crt0 calls it before `FlushCache` and main; it then calls `InitThread` `0x2c67b8`). Named in
   `recomp/shino_symbols_reviewed.txt`, and the runner given the symbol map: `_InitSys` is answered natively.
2. `jump to 0x614b80: no generated code`: the C++ static constructors, outside the code section (Build, above).
   Added as a second code section by tools/shino_prepare.py; all 21 run.
3. `no code at 0xffffff`: the stack. crt0 calls SetupThread with stack `-1` (the SDK's default `_stack`: "the end
   of RAM"), size `0x30000`; the engine returned `-1 + size`, an odd stack pointer inside the program, and the first
   return read garbage. Fixed in BozoEngine (`Kernel::threadSetup`: stack -1 is the `size` bytes below 32 MB).
4. `syscall 120` (`sceSifSetDChain` in `sceSifInitCmd`): `sceSifInitRpc` `0x2c8fa0` named, with the IOP reset
   (`sceSifRebootIop` `0x2cf110`, `sceSifSyncIop` `0x2cf0c0`), the module loads (`sceSifLoadModule` `0x2cebe0`,
   `sceSifLoadStartModule` `0x2cec00`) and `sceCdMmode` `0x2d3220` (S-command 0x22).
5. A bind loop in libcdvd: `sceCdDiskReady` `0x2d3000` (SDK 3.0's server `0x8000059c`, falling back to
   `0x8000059a`).
6. A bind loop on the fileio server `0x80000001`: `sceFsInit` `0x2ca868` and the fileio calls by their RPC function
   number through SDK 3.0's call wrapper `0x2ca1d0` (open 0, close 1, read 2, write 3, lseek 4, ioctl 5, getstat
   0xc). The game checks each file with `sceGetstat`, which BozoEngine did not answer: added to the engine's libcdvd
   (a disc file's size; the image opened first, as sceOpen does).
7. The game opens `cdrom0:\VOLUME.DAT;1` in a retry loop until getstat succeeds (6), then libpad's bind loop:
   `scePadInit` `0x2d37f8` and libpad named in TMB's order (`scePadPortOpen`, `scePadRead`, `scePadGetState`, ...).
8. `mfc0 28` in `_sceSDC`: `SyncDCache` `0x2c62d8` and `InvalidDCache` `0x2c6418`.
9. The game's own path reset `0x4454a0` (VIF0/VIF1/GIF FBRST, the VUs' through VI28): an override returning at once.
10. The first frame: `sceGsResetGraph` `0x2c1d28` and `sceGsSetDefDispEnv` `0x2c1fe0` named; libdma named by what
    each function does (SDK 3.0 shares one wait helper between them): `sceDmaSend` `0x2c3e80`, `SendN`, `SendI`,
    `Recv*`, `sceDmaSync` `0x2c4120`, `sceDmaWatch`, `sceDmaPutStallAddr`.
11. The render scheduler (below, "Drawing") and its VIF error masks in its set-up `0x444e40` (native sites).
12. A `switch` whose table base is loaded far from its `jr` (`0x4b5498`): bozo-recomp cannot find such tables;
    `tools/shino_entries.py` gives the targets of every function with an unresolved jump as entries
    (`recomp/shino_entries.txt`, `scripts/entries.sh`).
13. `sceSdRemoteInit` binds SDRDRV's `0x80000701`: the IOP sound modules compiled natively (below, "Sound").
14. The memory card check waits on `sceMcSync` `0x2d5f20` (with `sceMcEnd`, `sceMcMkdir`).
15. The movies (below), then the game's delays: SDK 3.0's timer library (`TimerUSec2BusClock` `0x2d1580`,
    `SetTimerAlarm` `0x2d1730`, `ReleaseTimerAlarm` `0x2d1950`, `DelayThread` `0x2c6a58`), answered natively by
    BozoEngine; guest code would wait for timer interrupts that never come.

Then the title screen and its menu, at full speed in the window.

## Drawing (bring-up path)

Shinobido builds each frame as a list of 8-byte commands its scheduler `0x4429d0` runs from the DMAC/INTC handlers
(word0's low byte: `0x01` VIF1 transfer, `0x02` GIF, `0x21` both with MSKPATH3 (the VIF1 chain with TTE), `0x40`/`0x41`
wait for the flip, `0x42` a counter's decrement, `0x4f` a call, `0x7f` a jump; chains or normal transfers by word0's
bits 8-9, QWC in 12-27). `runtime/shino_render.cpp` replaces it: the list is drained in one go, each transfer's DMA
chain walked; the vblank handler `0x442fb0` makes the flip (DISPFB1 / DISPLAY1 of the display environment, the field's
GIF chain) and publishes the frame. Nearly everything, the 2D included, is drawn by VU1 programs the game uploads with
MPG and feeds with UNPACK: `runtime/shino_vu1.cpp` reads the VIF1 stream (STCYCL, STMOD, STMASK, STROW/STCOL, the
TOPS double buffer) and runs the programs, recompiled (below, "VU1 microprograms, recompiled") or interpreted (pair
semantics: the lower reads before the upper commits; Q's latency; the MAC flags 4 pairs late; branch delay slots; the
E bit; vf0 the constant (0, 0, 0, 1)). XGKICK's packets (PATH1),
DIRECT's (PATH2) and the GIF channel's (PATH3) go to `runtime/shino_gs.cpp` / `shino_gs_state.cpp`: the GS registers
are kept, primitives become `Renderer::draw` triangles in window pixels (each vertex less its context's XYOFFSET, an
identity matrix), and the textures are decoded from a copy of the GS local memory the image transfers fill (the GS's block and
column arrangements, checked as permutations at start-up). Only the draws into the displayed frame buffer are shown.

This is a bring-up path, not the house style: the other ports answer the game's own draw functions natively with
data converted offline. It gets the menus on screen and gives a reference picture to replace it piece by piece.

Debug switches: `SHINO_TRACE_LIST` (the command list), `SHINO_TRACE_FLIP`, `SHINO_DUMP_PRIMS=<flip>`,
`SHINO_DUMP_KICKS=<flip>`, `SHINO_TRACE_PATH1=<flip>`, `SHINO_TRACE_VIF=<flip>`, `SHINO_TRACE_KICK=<flip>`,
`SHINO_DUMP_MICRO=<flip>` (VU1 memories at each program start, for `tools/shino_vu1_disasm.py`),
`SHINO_TRACE_PROGRAM=<flip>:<n>` (one program stepped pair by pair), `SHINO_TRACE_DRAWS`, `SHINO_TEST_QUAD`,
`SHINO_TRACE_PATH3=<flip>` (the GIF chains held behind MSKPATH3: DMA pieces and GIF tags),
`SHINO_PICK=<flip>,<x>,<y>[,<x>,<y>...]` (every triangle over those window pixels, in drawing order: texture, TEST,
UVQ, Z), `SHINO_DUMP_TEXTURES=<flip>` (each texture decoded from that flip on as `build/tex/<tex0>_<w>x<h>.rgba`).
Hidden runs: `--hidden --mute` (offscreen, no sound).
Screenshots: `PS2X_CAPTURE_SCENE=1 PS2X_SCREENSHOT_AT=<s>,... PS2X_SCREENSHOT_DIR=<dir>` (the offscreen picture: the
back buffer reads black when the window is not visible).

## Sound

LIBSD, SDRDRV, TORAST and TORASE are statically compiled (`tools/shino_bozo_audio.py`, BozoEngine's
`tools/native_irx`) and run by the engine's compiled-library host (`runtime/shino_audio.cpp`); TORAST streams the
music archives through the host's `ioman`. The pad, memory card, CD streaming and USB modules are answered at their EE
APIs.

The title's music first came out as noise (a capture: from 6.5 s a steady level, zero crossings at ~23 kHz, values
with no structure). TORAST allocates its music buffers down from the last byte of sound memory
(`0x1fffff - 0x28000 = 0x1d7fff`) and passes that odd address both to `sceSdVoiceTrans` and to the voice's SSA. The SPU2
takes both in units of 8 halfwords; BozoEngine's mixer aligned the voice's start but `sceSdVoiceTrans` wrote at the
exact byte, so every ADPCM block decoded 15 bytes off. Fixed in BozoEngine (libsd: the transfer address in 16-byte
units, PR TheBozos/BozoEngine#127); the capture now has the level and zero crossings of music.

### In play (2026-11-24)

TORAST owns RPC server `0x12345`, TORASE (the effects) `0x11111`. In play the game streams ADPCM to TORAST: command
`0x80e0`, four 348-byte pieces a frame, which play as one stereo pair of voices (the music, and the cutscene's sound).
It hands TORASE a 308-byte block each frame (command `0x20000000`). Effects key voices on as the ninja acts (sword,
jump: 3 to 5 key ons every 50 frames), few while he stands or walks. In the pause menu the game freezes the stream: its
two voices hold their last sample (a constant level in a capture, as the SPU2's would). Loud effects over the music
clip now and then (up to about 100 samples a second).

Debug switches: `SHINO_TRACE_SOUND=<n>` (every n frames: key ons, voices playing, the peak; each core's input, its volumes, the master volumes and the mix switches), `SHINO_TRACE_RPC=1` (the
drivers' RPC calls by service and command every 250 frames; command | 1 counts the calls whose send buffer is not all
zeros), `SHINO_TRACE_RPC_DUMP=<frame>` with `SHINO_TRACE_RPC_SERVICE=<hex>` (16 calls' send buffers from that frame).
`--wav FILE` records the mix in the game's own time (headless too).

## Movies

libmpeg is named (`sceMpegInit` `0x33d040`, `sceMpegCreate` `0x33c498`, `sceMpegDemuxPssRing` `0x341330`, ...) and
decoded natively (FFmpeg); the pictures come through `bozo::render::setMoviePictureHook`, and the movie's vblank
handler `0x326860` swaps with `sceGsSwapDBuff` (answered by the port: the picture published as the backdrop, the
frame ended).

The sound: the player (`0x327410`, on the main thread) demuxes the PSS from its `0x50000`-byte read ring. Its PCM
callback `0x327f30` copies each audio packet (4073 or 4059 bytes: odd sizes) into a `0xc000`-byte EE ring
(`0x672540`), and returns 0 when that ring has no room; a thread sends it to the IOP's `0x6000`-byte sound buffer
in 1 KiB blocks through `sceSdRemote` block transfers (`0x325ce0` / `0x325f10`), placed by the SPU2's transfer
address (`sceSdBlockTransStatus`). The player starts showing pictures once that buffer has its first `0x6000` bytes.
The port's settings in BozoEngine:
- `picturesAhead = 32`: the first audio packet comes 14 pictures into the stream, so the demux reads that far ahead.
- `sdkDemux`: each packet's callback runs before the demux goes on, and a refused sound packet stops the demux
  before it (the next call gives it again), as the SDK's libmpeg does. Without it a refused packet was lost, and the
  sound after an odd-sized one was read a byte off: noise, from about 28 s into the intro. The video callback's 0
  is not waited for: the native decoder takes the video from the demux, and the game's own IPU input ring
  (`0x6724c8`) is never drained.
- `useBlockTransStatusAhead` (the sound host, `runtime/shino_audio.cpp`): libsd's `sceSdBlockTransStatus` reports the
  transfer address one 1 KiB block ahead of the sample playing, as the SPU2 does: at the play position itself the
  player's ring read as empty once full, and was overwritten.

The Acquire logo (`ACQUIRE.PSS`) is silent on the disc: its sound track (48 kHz stereo, 512-byte interleave, as
`NINJA.PSS`'s) is 895K samples of zero. The port feeds it to the SPU2's input like the others (core 0's input, its
volume 0x7fff, the master 0x3332); `NINJA.PSS`'s track peaks at 32387 and plays.

`SHINO_SKIP_MOVIES=1` answers `0x327130` as a finished playback (each movie skipped); `SHINO_TRACE_MOVIE=1` prints
the sound feed (`0x672540`: its EE ring bytes, the IOP buffer's write position, the bytes sent) every 25 fields.

## 3D (New Game to gameplay)

"New Game" plays the story movie `S110.PSS`, loads the castle and runs the opening in-engine cutscene, then gameplay.
What it took:
- **VU0 microprograms.** The 3D (collision, culling) calls VU0 programs with VCALLMS at 19 entries (no VCALLMSR;
  `tools/shino_vcallms.py` lists them), from microcode the game sends to VIF0. The VU1 interpreter in
  `runtime/shino_vu1.cpp` serves VU0 too: a second unit with VU0's 4 KB memories, its registers copied from and back to
  the EE's COP2 state around each program, its data memory the runtime's (the EE reads and writes it at 0x11004000).
  Each entry is registered with `Registry::vu0Program` (`runtime/shino_render.cpp`).
- **VIF0.** The microcode loader `0x231ec8` (a VIF0 chain, TTE, skipped when it is the chain loaded last at `0x523228`)
  is overridden: its chain's MPG and UNPACK codes go to the port's VU0. Two other senders (`0x1ec118`, `0x21c8a0`) set
  CHCR's TTE and call `sceDmaSend(VIF0, chain)` inline: native sites drop the CHCR read and write and hand the chain
  (`0x4f1500`, `0x4f1a80`; `tools/shino_vif0_micro.py` extracts their microcode) to the port's VU0 where the call's
  delay slot makes its address. libdma's native send completes the transfer.
- **VU0 code memory.** The collision search `0x1ebb30` indexes the VU0 data arrays with the lane its last round chose
  (`0x1eadb8`), -1 when none, so it reads `0x11003fec`, just below VU0 data memory, inside the code memory's mirror.
  The console allows it; the port gives its VU0 code memory to the host (`Registry::vu0CodeMemory`, BozoEngine #130)
  so those reads reach it.
- **VU branches wrap.** The 3D's VU1 program branches back from `0x18f0` to `0x3978`, past the start of code memory:
  a branch target is taken modulo the code memory's size. Before that the program fell through and ran into the next
  routine for 4M steps (the "did not end" messages), at under a frame a second.

Debug switches: `SHINO_TRACE_VU0=<n>` (the first n VU0 programs: entry, VI registers, the data memory's nonzero
quadwords, VF registers after), `SHINO_DUMP_MICRO=<flip>[:<first>]` (from the flip's first-th program),
`BOZO_FAULT_REGISTERS=1` (the registers with a fault). The "did not end" message names the flip and the program's
index in it, for `SHINO_TRACE_PROGRAM`. Pad scripts drive the runs: `--pad-script FILE`, entries
`<frame>:<BUTTON>/<frames held>` (`1000:CROSS/6` picks "New Game" on the title; START skips a cutscene).

## 3D textures and depth

Three things made the 3D's surfaces wrong (flat colours, green walls, rainbow stripes, streaks):
- **PATH3 order.** The textures go through the GIF channel (PATH3) in the same list op as the VIF1 chain that draws
  with them (`0x21`). The GIF chain holds one EOP packet group per texture (palette, image, TEXFLUSH); the VIF1 chain
  lets each through just before the draws that use it with FLUSHA, MSKPATH3 0, MSKPATH3 1 (143 groups, 144 unmasks a
  frame at the castle). Sent whole before the VIF1 chain, the textures sharing the streaming area (TBP `0x2800`)
  overwrote one another before anything was drawn. `runtime/shino_render.cpp` now holds the GIF chain and sends one
  group at each unmask (`path3::unmasked`, from VIF1's MSKPATH3); what is left goes after the VIF1 chain.
- **Depth.** The 3D tests GEQUAL on a Z buffer (TEST `0x5140b`): up to 256 triangles cover one floor pixel, buildings
  behind it included. The draws now go through `Renderer::draw` in submission order with GS Z per vertex, TEST's
  ZTE / ZTST (ALWAYS no test, NEVER not drawn), ZBUF's ZMSK for the write and its PSMZ for `Screen::depthMax`.
- **The alpha test's failing pixels.** TEST's AFAIL is FB_ONLY: a texel below AREF 0x40 is still drawn, without its Z.
  Such a batch is two passes: the test as given, then the opposite test without the Z write (ZB_ONLY: Z and no colour;
  RGB_ONLY: no alpha, no Z).
- **Perspective.** STQ draws hand the GS's Q to the renderer (`Material::useQ`), S / Q and T / Q interpolated in
  perspective as the GS does.

## Speed

The game runs at 25 frames a second (PAL pacing, below), and play holds it in the window: about 40 frames a second
of headroom headless. A gameplay frame was about 66 ms (2026-11-23); what it took, by `SHINO_PROFILE=1` (each part's
time a flip, exclusive; `SHINO_RATE=1` the rate alone):
- **VU1** (1.9M instruction pairs a frame in play, 3.4M at the castle's load): recompiled since (next section). The
  interpreter, for what the compiled code hands it: each pair decoded once (`Decoded`, checked against the raw words, so
  a new MPG needs no invalidation); the upper's arithmetic on whole vectors (SSE: MAX / MINI as `std::max` /
  `std::min` on ±0, NaN 0, clamped), the common lower instructions from the decoding (`lowerFast`), the rest through
  the scalar `upper()` / `lower()`. `SHINO_VU_SCALAR=1` runs the scalar path, `SHINO_LOWER_SLOW=<mask>` the lower
  kinds in the mask through `lower()`: the checks.
- **VIF**: codes counted in an array (a `std::map` before); a command's data used in place when the chunk holds it all;
  UNPACK's common case (no mask, no fill, STMOD 0, whole-byte elements) without the bit reader.
- **Uploads** (all CT32 or CT16, about 1 MB a frame: the 4- and 8-bit textures go up as 32- and 16-bit pixels):
  gathered and written when complete, by whole GS blocks (a compare and a copy of 256 bytes) when aligned, by rows
  otherwise; a page is marked written only when a value in it changes.
- **Textures**: decoded ones also kept by the hash of the bytes they come from (the game streams its textures through
  a shared area every frame: swapped back in, a texture is not decoded again); the last primitive's texture reused
  while nothing was written. Decodes down about four times.
- **The GS translator**: the materials made again only when their registers change; primitives of the same material
  and texture merge into one batch, a two-pass (AFAIL) run keeping its passes in two (before, two batches a triangle).
- `SHINO_CHECKSUM=1`: a hash of every vertex drawn, every 500 flips; all the changes above keep it, through the boot,
  New Game, the cutscene and play.

## VU1 microprograms, recompiled (2026-11-24)

The game keeps all its VU1 code in its executable, as DMA packets ready for VIF1: a tag (cnt or ret) whose data are
only NOP and MPG codes, each MPG's instruction pairs after it. It sends one before the programs that use it, about 25
a frame in play (`SHINO_VU_SURVEY=1`: the MPG uploads by EE address, and the programs run, by start and the code
loaded). The 3D sets fill pairs `0x000` to `0x7ff` from 256-pair MPGs; small 2D sets load over the start of one.

`tools/shino_vu1_recomp.py` finds every such packet in the player's executable by its shape (123 sets, 126,853 pairs)
and writes one C++ function per set into the build (`build/bozo/shino_vu1_gen/`, eight files; nothing from the disc in
the repository). The compiled code runs the interpreter's own operations (`runtime/shino_vu.h`, shared with
`runtime/shino_vu1.cpp`), pair by pair with constant operands: the upper computed before the lower, committed after
it; the branches' delay slot; the E bit; FMAND / FMEQ / FMOR on the MAC flags of 4 pairs earlier. What no instruction
can see is left out, from the control flow of the set:
- Q's countdown, where no DIV / SQRT / RSQRT result can still be pending (forward, the longest countdown at each pair);
- the MAC flags' history, where no FMAND / FMEQ / FMOR reads it within 4 pairs and the program goes on in the set;
- the MAC flags themselves (and STATUS's two bits from them), where the next result replaces them before any read.
The rarer instructions (XGKICK, the EFU, FS*, the R register) go through the interpreter's functions. A branch in a delay slot, or an E bit on a branch, would go to the interpreter (none in this game).

At run time (`runtime/shino_vu1.cpp`): a program runs in the set whose code is the one loaded at its pc, checked by
64-pair segment and checked again only after an MPG changed the segment (`vu::segmentOk`); where the program goes on
in another set's code (the 3D sets' last pairs stay loaded under the next set), it moves to that set. Every pair is an
entry (a program's start, JR's targets), with one condition: Q's countdown no longer than the code there counts down;
otherwise the interpreter runs the pairs until a set can take the program back.

The registers are the compiled function's locals (`f1`-`f31`, `fa` the ACC, `i1`-`i15`, Q's value, its countdown
and pending value, I): loaded from the unit on the way in, stored back on the way out (one exit) and around the
interpreter's functions (`SHINO_LOAD` / `SHINO_STORE`). The data memory's stores can no longer alias them, so the
compiler keeps them in registers or its own stack slots instead of reloading the unit after every store, and a dest
field's lanes are a constant mask. The operations on values (`arithV`, `blend`, `opmulaV`, `itofV`, `clipV`,
`divideResult`...) are the ones the interpreter's forms on the unit call. The VU1's part of the castle's load went
down by about a third (the game thread's CPU time a frame 18.5 to 16 ms, the cutscene 10.3 to 9). The generated code
takes about 4 minutes to compile now (1.5 before).

Checks: `SHINO_VU_VERIFY=1` runs each program compiled and then interpreted from the same state and compares
everything (registers, flags, Q's countdown, the history's last 4, data memory, TPC, kicks): the New Game run's 728k
programs and the play from the frame-5200 save state, none different. The vertex checksums (`SHINO_CHECKSUM=1`) are
the interpreter's. `SHINO_VU_INTERP=1` runs the interpreter alone.

Speed: at the castle's load the VU1 takes 8 to 10 ms a frame (3.4M pairs; about 30 ms interpreted). Headless, the load
and the cutscene's start went from 12 to 17 frames a second to 30 to 36, play from 29 to about 60. In the window, the
load and the cutscene's start hold 24 to 25 frames a second (16 to 17 before), and play near several guards 24 to 25
(down to 19.5 before). The GS translator (10 ms a frame at the load), VIF (6 ms), the texture decodes and uploads were
then the larger parts (next section).

## The GS translator and VIF, faster (2026-11-24)

Measured with `SHINO_SAMPLE=<first flip>:<last flip>` (`runtime/shino_profile.cpp`: the game thread's instruction
pointer every millisecond, written to `build/samples.txt`; for a sample in a DLL, the executable's function that
called it) and `tools/shino_samples.py <executable>` (the samples by function, from the symbol table and the DLLs'
exports); `SHINO_RATE=1` also prints the game thread's CPU time a flip, steadier than the rate while other programs
load the machine. At the castle's load (flips 1600 to 2100 of the New Game run), what was found and done:
- **getenv a VU program.** `SHINO_TRACE_PROGRAM` was read at each program's start, 1,300 a frame: 12% of the game
  thread (Windows' getenv is slow). Read once.
- **Textures decoded again and again** (86 a frame at the load, 37 in the cutscene): the hash that finds a texture
  decoded before covered its buffer-width rows and 8 KB more, and the whole palette area, and was seeded with where
  the texture is. The game streams its textures through shared areas, next to other textures and palettes that
  change, and the same texture into different slots. The hash is now of the GS blocks the texture's pixels are in, the
  palette's colours it uses, and what the picture depends on (format, size, TEXA), not where it is: 0.4 decodes a frame
  at the load, none in the cutscene. The 64 textures used last are kept (a 3D frame alternates between a few between
  uploads), and a texture's pixels are decoded by one loop per format.
- **UNPACK** (200k vectors a frame: V2-32, V3-32, V3-8, V4-8 unsigned, CL above WL): one function per element shape,
  the destination stepped (it was two divisions a vector).
- **Draws**: the frame's vertex vectors kept from frame to frame (they were freed at each flip and grown again); no
  copy of the texture's shared pointer a primitive; the frame buffer's list kept from one primitive to the next; the
  primitive's vertices pushed one by one (a memmove call a primitive before); the vertex queue a fixed array; an
  XGKICK's PACKED data run in the translator's own file (no call a quadword).
- **VU1**: OPMULA / OPMSUB (the 3D's cross products) inline in the compiled code (they went through `upper()`).

Then the uploads and the draws (`SHINO_PROFILE=1` also prints the uploads a flip: about 200 at the castle's load, 1.3
MB, nearly every 256-byte block of them changed: the game streams different textures through the same areas each
frame):
- **The held GIF chain** (PATH3 behind MSKPATH3, `sendHeld`) went to the translator one quadword a call, so an image's
  pixels came to the upload 16 bytes at a time and were always gathered into a copy. A tag's data now goes as one run
  (as far as the DMA piece holds it): an upload whose pixels arrive at once is written from the DMA data, no copy.
- **The whole-upload hash** (to skip an upload of the bytes already there) never skipped one in 3D: gone (each block
  is still compared, and a page is marked written only where something changed).
- **Blocks** (CT32 8 x 8, CT16 16 x 8) put together with SSE2 unpacks of their rows (a CT32 row's pixel pairs are
  8-byte runs, two rows interleaved; a CT16 row's pixels p and p + 8 alternate), each 16 bytes compared as it is
  stored; checked against the column tables at start-up (`writeBlock32` / `writeBlock16`).
- **The textures used last** stay good after an upload elsewhere: the generation over their own pages compared.
- The vertex queue's shift spelled out (GCC made the loop a memmove call).

The game thread's CPU time a frame (headless): at the load 32 ms before (the VU1 recompiled), about 15.5 now; in the
cutscene 18, about 7.5. The GS translator and VIF take about 5 ms of the load's 17 (the VU1 about 6.5); the renderer's
own copy of the vertices (BozoEngine's `Renderer::draw`) about 2.5%. Vertex checksums unchanged.

## The frame-end wait (2026-11-24)

`0x443640` ends the game's frame: after its DMA set-up it waits (`0x443728` to `0x443760`) while the frame counters
`gp-0x6810` and `gp-0x6814` differ or the byte `gp-0x6800` is set (the vblank's handler changes them), calling the idle
callback `gp-0x6854` each pass when there is one. Each pass charges 32 EE cycles at its checkpoint: the wait was 1.07
billion passes over the New Game run (346,000 a frame), up to a third of the game thread's time in the menus. The
callback is never set in this game (the port's count: none in the New Game run or in play).

A native site at `0x443728` (the poll's first load, `runtime/shino_port.cpp`) makes the load, and when the loop will go
round again with no callback, charges the clock to the next scheduled event (`Kernel::nextEvent`: the vblank's start
or end, a native deadline), as the dispatcher does when every thread waits: the next checkpoint takes the vblank,
which the loop would have reached pass by pass. 13,815 passes over the run instead. The vertex checksums are the same
through the New Game run and 3,000 frames of play from the save state (`SHINO_NO_WAIT_SKIP=1` runs the loop as it
was). The game thread's CPU time a frame: the menus 1.0 to 0.3 ms, the castle's load 14 to about 11 (headless); in
the window the load and the cutscene hold 25 frames a second at 14 to 24 ms of the 40.

## Scissor, FBMSK, the frame as a texture (2026-11-24)

`SHINO_SURVEY=1` counts the primitives by what the renderer was not doing, every 250 flips. From the boot through the
cutscene and play:
- **The previous frame, blended over the new one.** Each frame one full-screen triangle fan (TEX0 `0x264020000`: CT32,
  512 x 512, at the other displayed buffer; ALPHA `0x44`, the vertex alpha) draws the frame shown last over the frame
  being drawn: an afterimage (the cutscene's moving figures trail). The port's GS memory has no rendered frames, so the
  fan blended old memory (black) instead, which darkened the sky. A CT32 / CT24 texture at a displayed frame buffer
  other than the one drawn into is now the renderer's previous frame (`Material::fromFrame`, `kPreviousFrame`), u and
  v across it by TEX0's size.
- **FRAME's FBMSK.** About 16 textured primitives a frame in play mask all 32 bits (depth only): a channel whose 8 bits
  are all masked is not written (`Material::colourMask`).
- **Scissor**: never narrower than the frame in these scenes. **Offscreen draws** (into a buffer never displayed): none.

## Play past the first area (2026-11-24)

From a save state at frame 5200 (`--save-state FILE --save-state-at 5200` on a New Game run; the game uploads its
textures again each frame, so the state's missing GS memory is back after one frame), scripted runs (`--load-state`,
the pad script and `--frames` in the state's own frame numbers) went through the castle grounds: running, wall climbs,
jumps, the second tutorial message, guards that spot the ninja and fight him, his health going down. 13,700 frames
headless (about nine minutes of play) and 14,000 in the window: no fault, no unknown device access, no unanswered
call. Play holds 25 frames a second, down to 15 to 18 for a while near several guards.

The pause menu's screens open and draw: Check Mission (three pages: the mission's picture and story, then its type,
destination and requirements), Controls (the pad's diagram), Settings (sound, the BGM / SE / voice volumes, aspect
ratio, brightness, blood, vibration, camera). Settings offers a 16:9 picture ("16:9 (Widescreen)"): the game renders
it anamorphic, so the port's 4:3 window would squeeze it; a 16:9 window for that setting is still to do.

To look at with a reference: a light disc under a crouching guard (dust, or a shadow blended the wrong way?), and the
strong yellow light on some walls.

## Next

- Speed: the castle's load takes about 15.5 ms of CPU a frame (40 ms at 25 frames a second). The compiled VU1 code is
  the largest part (about 1.5 ns a pair: the arithmetic and its clamping). Then the renderer's own copy of every
  vertex (BozoEngine's `Renderer::draw`, about 4.5%: an engine change) and the primitives' assembly (`emit`, about 6%).
- 3D: native draws at the game's model functions (the bring-up path draws the GS packets).
- Sound: a listen to the cutscene's spoken lines and to the effects in play (the captures show the stream and the
  effects' voices, not what they sound like).

## PAL pacing

The game resets the GS with video mode 0 (`sceGsResetGraph`): the console's own standard. A PAL console's is PAL, so
the port sets BozoEngine's `palConsole` (fields at 50 Hz for mode 0, as for mode 3).

The game flips every second field: its vblank handler `0x442fb0` counts vblanks up to a minimum of 2 (`gp-0x6850`)
before it flips, at the title and in play alike (`SHINO_TRACE_VBLANK=<flip>` prints the count). On the console it runs
at 25 frames a second; a host frame (one flip) spans two fields of the game's time (11.8M EE cycles). The window used to
hold the game to 50 frames a second, which ran it at twice its speed wherever the port kept up (the boot, the menus).
It now holds each frame to the fields it spanned (`runtime/shino_main.cpp`, the cap 50 / fields: 25 in this game);
`PS2X_FRAME_CAP` overrides it.
