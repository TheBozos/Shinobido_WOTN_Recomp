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

The demux copied the stream into its pending buffer a byte at a time (a guest translation each, 0x800 a sector); the
ending's and the credits' long movies were decode-bound, about 3 minutes headless. It copies whole runs from the ring
now (BozoEngine `appendStream`, 2026-11-26): 44 s, the same pictures and flips.

The movies still played at about 60% speed in the window, sound or none (80% headless; the 42 "new pictures" a second
the window counted were the vblank handler's backdrops, one a field, not movie pictures; `BOZO_TIMING_LOG=1` prints
the pictures' wall time, vblank and EE time). Each field took 23 ms of the host's time: 65,000 dispatches, 56,000
stops and 9,300 handlers, nearly all the PCM callback `0x327f30`. With `sdkDemux` a sound packet the callback refuses
(the EE ring full) ends the demux call and is offered again by the next, as the SDK does; on the console the player's
turn between two calls takes time, here none, so the player spun until its sound thread made room. A demux call after
a refusal now costs 0.2 ms of EE time (BozoEngine `kRefusedDemuxCycles`, TheBozos/BozoEngine#158): 102 handlers a
field, 1.4 ms of host time, 25 pictures a second in the window, the pictures' EE timing unchanged (30 every 60
vblanks), the intro's sound without a gap. The fight into the ending and the credits: 27 s headless.

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
- `SHINO_CHECKSUM=1`: a hash of every vertex drawn, every 500 flips (`SHINO_CHECKSUM_EVERY=n`: every n); all the
  changes above keep it, through the boot, New Game, the cutscene and play. The native 3D batches have their own,
  printed after it (`native`).

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

### VU0 compiled too (2026-11-26)

The VU0 microcode (the packet at `0x4f2a70`, 478 pairs) is one of the 123 sets, but ran interpreted. It runs compiled
now: each unit keeps its own segment versions and checks (a VU0 MPG no longer invalidates the VU1 sets, nor back),
the set lookup keeps to the unit's code size (pcMask), and the compiled loads and stores address the unit's own data
memory (`mem1`: `v.data`, `qwMask`; it read VU1's before, which `SHINO_VU_VERIFY=1` showed as zeros loaded in the
programs at `0x520` and `0x4e8`). Verify names the unit and prints the differing registers (compiled, interpreted,
at entry). None different over 600 frames of VU0 code and 500 of play (347k VU1 programs). `SHINO_VU0_INTERP=1`
interprets VU0 again. A small gain: 1 to 2 ms in the heaviest flips.

### A busy fight, measured (2026-11-27)

Where the port falls short: the uncapped window runs of the missions (`build/cyc/*/run.txt`, `[window]` lines). Every
stretch under 22 game frames a second was a movie (the demux, above); gameplay dipped to 21 to 33 only while other runs
loaded the machine. Replayed alone, the busiest (r94: a town at night, guards fought, from `q93.state` with its pad and
teleports) never goes under 52 game frames a second uncapped (the game needs 25). Sampled there (flips 6000 to 8750):
- **VU0's calls.** The game calls about 10,000 VU0 programs a frame, of about 7 pairs (a collision loop, `0x1eadb8`,
  calls `0x4e8` and `0x4b0` each turn). `vu0::run` copied the 31 vf registers in and out around each: 15% of the game
  thread, mostly stalls on the copies. The VU0 unit now uses the EE context's vectors in place (`Unit::vf` a pointer:
  VU1's own `regs`, VU0's `ctx->vu0_vf`); vi, ACC and the rest are still copied. 2%.
- **MMI.** The same loop's pcgtw / pmaxw / paddw were scalar loops through the stack (BozoEngine's `ops_mmi.h`, lane
  loops over the union): 10%. They are SSE2 now (TheBozos/BozoEngine#155, 42 helpers, checked against the loops on
  8.4M random cases); the function no longer shows.
Together the stretch's samples went from 20,921 to 15,534 (26% less CPU), the VU0 and VU1 program and pair counts the
same; `SHINO_VU_VERIFY=1` from `q93.state` into the fight: 52M programs, none different. What is left there: the
compiled VU1 code 35% (images 45, 0 and 67: the clamping, the dest blends, the pair count and Q's countdown each pair,
spills of the 32 vector locals), the EE code 26% (spread), the VU call path 9%, the GS translator 7%, VIF 6%.

Then the VU call path: about 90 times the VU0 code's own time (`shino_vu1_image_2`: 13 samples, the chain to it
1,160). Trimmed: VU0 programs run the image last found at their pc directly (`runVu0Compiled`, without
runProgram's VU1 bookkeeping; SHINO_VU_VERIFY, SHINO_VU0_INTERP and SHINO_TRACE_VU0 take the full path), `vu0::run`'s
vi copies kept scalar (a 16-byte load of halfwords just stored one by one stalls), and BozoEngine finds the port's VU0
programs in a table by pair instead of a search (TheBozos/BozoEngine#157). The chain is about a sixth shorter (other
programs on the machine made the totals vary by a third between runs); the vertex checksums over 8,500 flips are those
of VU0 interpreted. What is left of it is the calls themselves (the EE code to the engine's services, the host, the
port's entry) and the segment check. The compiled VU1 code stays the largest part: over SSE2 its cost is mostly the
VU's float rules (NaN 0, the clamp; the dest blends three logic operations; GCC already drops a blend of all four
fields), so a gain there would need SSE4.1 (one-instruction blends), a requirement the port does not have.

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

Then the primitives' assembly (30,000 primitives a frame at the castle's load, nearly all triangles from XGKICK's
PACKED tags):
- **A tag at once.** A PACKED tag whose registers are only ST, RGBAQ, UV, FOG and XYZ (and NOP) cannot change what its
  primitives are drawn with: PRIM, the context, the texture, the materials, the batch are found once, at its first
  triangle drawn (`packedFast`), and each triangle is made into the batch as its vertices come, with no call a
  register, a vertex or a primitive. Other tags, and every tag while a primitive trace is on, go register by register
  (`packedOne`, `vertexKick`, `emit`); both share `makerFor` (the texture, U / V's scale) and `targetFor` (the
  materials and the batch).
- **U / V by multiplication.** U / 16 / width with the width a power of two is U times the exact 1 / (16 x width): the
  same bits, without the divisions.
- `emit`'s triangles are made into the batch (no copy from a scratch vector).
The vertex checksums are the same through the New Game run and 3,000 frames of play; the castle's load about 10 to 9.5
ms of CPU a frame (the machine less loaded than for the numbers below).

Then the renderer's copy: the batches' vertices were copied into the renderer's frame at the flip (`Renderer::draw`,
3 to 6 MB a frame at the castle's load). BozoEngine's renderer now takes draws assembled in place
(`appendVertices`, `drawRange`, `continueFrame`): the primitives' vertices are made straight into the frame being
built, a batch is a range of them drawn at the flip, a two-pass draw's two batches share one range (no second copy
either), and a batch grows only while its vertices are the last made. A movie's picture, a frame the port starts
itself, drops the game's draws first (`gs::dropDraws`: beginFrame empties the vertices). The copy's 2 to 4.5% of the
game thread is gone (`appendVertices` and `drawRange` about 1.7% together); the vertex checksums and a gameplay capture
are the same.

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

## Cheats (2026-11-24)

`runtime/shino_cheats.cpp`, applied at each frame's end on the game thread; the window's Cheats tab (F2,
`runtime/app/shino_cheats_ui.cpp`), or `SHINO_CHEAT_HEALTH=1` and `SHINO_TELEPORT=<x>,<y>,<z>` without one. The
executable is stripped, so the fields were found in the game's memory, from the frame-5200 save state and the scripted
play (`SHINO_DUMP_RAM_AT=<frame>,...` writes the RAM at those frames to `build/ram_<frame>.bin`,
`SHINO_TRACE_PLAYER=<n>` the player's object every n frames to `build/player_trace.bin`, `SHINO_POKE=<frame>:<address>:
<x>,<y>,<z>` writes three floats):
- **The player's object**: the pointer at `gp-0x6a0c` (`0x625d64`; `gp-0x6a08` and `gp-0x6308` hold it too). Its first
  word points at itself.
- **His position**: the translation of his world matrix (+0x10: rows x, y, z, then the translation at +0x40). Written
  there, the game moves him on from it; the copies at +0x140, in the draw list (a 48-byte record a model) and in two
  more places are the last frame's, written from it. Found by the floats that move steadily while the script holds UP,
  then poked one by one: only +0x40 moved him. The z row (+0x30) is the way he faces. The camera is the matrix the
  pointer at `gp-0x6d44` gives (its translation also at `0x679c70`); `0x521ba0`, 100 units ahead of it, is not him.
- **His health**: +0x64c, an int, out of +0x650 (100 in the first mission): the one field of his object that only went
  down, 100 to 5, through the scripted fights (from frame 9350).
- **The characters**: a table the pointer at `gp-0x6d38` (`0x625a38`) gives: 16-byte entries from +0x20, the
  character's object at +4 (the player first, then the guards: 11 in the first mission). Found from the draw list's
  records (the object pointer 16 bytes before each model's position) and the heap block that held them all in order.
  A guard's object has the player's layout (itself at +0, its world matrix at +0x10) and the same status block, which
  starts with the object's address again: at +0x630 for the player, +0x620 for a guard; health at +0x1c in it, out of
  +0x20 (a guard's 24, the player's 100). The guard the script fights went 24 to 19 under one hit.
- **One-hit kills** (`SHINO_CHEAT_KILLS=1`): every other character's health above 1 is set to 1 each frame, so the next
  hit takes it below 0 and the game's own death follows (the dead, at 0 or below, are left alone).
- **Guards never spot the ninja** (`SHINO_CHEAT_UNSEEN=1`). A guard's AI is a "brain" object (its class's table at
  `0x61af90`, the guard at +8; the one the script fights is at `0x1e43790`). It keeps its stimuli in three queues of
  0x30-byte records (+0x394, +0x3a4, +0x3b4; each `{source, state, -, -, position, priority, kind}`). Each frame its
  perception (`0x167540`) takes the first (`0x16b950`) into its record at +0x10, and `0x16d5e0` copies that to +0x40:
  its target, what the fight goes after (`0x34ddb0` reads it, and raises "seen" from it each frame). Every stimulus goes
  in through one push, `0x16b3a0(brain, queue, source, priority, kind, position)`. Its first step, `0x16b8c0`, refreshes
  the queue's record of that source and answers 1 when there was one, and the push then adds nothing. The native site
  at its return (`0x16b93c`) answers 1 for the player when the cheat is on. His "seen" events are dropped too: the
  character event `0x112d60` (type 17, +0x3e0 the source) is overridden. What the guards hear is not his: a no-source
  "go and look" record (state 4) from their noise list (+0x2e0, `0x16ebd0`, `0x169ae0`). They still walk to it and never
  find him. Found by a write watch on the brain's records: they are filled by memcpy, from records on the stack.
  The earlier leads were wrong. `0x12b7a0` / `0x12ba70` is the player's lock-on, not a guard's sight. A guard's +0x580
  holds animation slots. Blocking the "seen" event alone changes nothing. `SHINO_TRACE_STIMULI=1` prints each new
  stimulus and the push's callers. `SHINO_TRACE_SEEN=1` and `SHINO_TRACE_EVENTS=<character>` print the events.
  `SHINO_TRACE_CHARACTERS=<n>` writes every character's object (4 KB each) every n frames to
  `build/characters_trace.bin`.
Checked headless: `SHINO_TELEPORT=60,0,250` from the save state puts him there and he walks on; with
`SHINO_CHEAT_HEALTH=1` his health stays 100 through the fights that took it to 5; with `SHINO_CHEAT_KILLS=1` the
guard the script fights goes from 1 to -4 at the hit that took him to 19, and never moves again. With
`SHINO_CHEAT_UNSEEN=1` (12000 frames from the save state), that guard's brain never holds the player. Every one of his
stimuli (144) is dropped, and his health stays 100, where without the cheat the guard's hits take it to 81 from frame
9350. Unchanged with the cheats off (the boot's checksums, 3100 frames). The tab is drawn only in the window (not looked
at yet).

## Save states and the sound (2026-11-24)

A loaded state left the sound modules' IOP host as it was (the port never gave it to the state): silent from the
load on (no key on, master volume 0), and the RPC servers the modules had registered gone. The EE's calls to them
were answered with nothing, and nothing showed it until a movie played: `S126.PSS` (a story movie a teleport set
off) waits for its sound buffer through `sceSdRemote` (`0x2d5080`, SDRDRV's `0x80000701`). Its block transfer
(`0x80e0`) never reached libsd, so `sceSdBlockTransStatus` (`0x8100`) never moved, the player's EE ring stayed full
and the demux spun (`demuxOn`) with no frame ended for good, 22 pictures in. The port's state section `shino`
(`runtime/shino_audio.cpp`) now holds the IOP host, as Onimusha's does. From a state saved at frame 1500 (the title),
the music plays on after a load (2 voices, master 0x3fff) where it was silent. States saved before this have no
`shino` section and do not load.

Play tests: `SHINO_CAPTURE_FRAMES=<first>:<last>:<step>` (or a list) saves the window's picture at those game frames
into `PS2X_CAPTURE_WINDOW_DIR`; `SHINO_TELEPORT_AT=<frame>:<x>,<y>,<z>;...` teleports at those frames. With
`PS2X_FRAME_CAP=0` the window runs as fast as it can. A fall from 400 units above the grounds kills the ninja whatever
his health (the game's own fall, `Cut_PlayerFall`): infinite health keeps the number, not him.

## Past the first mission (2026-11-24)

With the cheats, from the frame-5200 save state: `SHINO_TELEPORT_AT=5300:250,400,-200` drops the ninja onto the castle
keep (the mission's "Go to the castle keep"). The game completes the mission and plays `S126.PSS` (the meeting with
Lord Ichijo, about 2,400 frames, with its sound), then goes on to the Hideout. Seen there, all drawn and working:
- the Hideout's help (three pages: Triangle closes the last) and its menu over the 3D hut: Mission Scroll (Missions,
  Letters, Results, Advice), Equipment (Equip, Buy Items, Sell), Options (sound, aspect ratio, brightness, blood,
  vibration, camera), To Garden (Enter Garden, Edit), Next (Check Status, Save Game, End Game);
- Save Game: a new save made and overwritten on memory card slot 1. Since BozoEngine #141 (`290e689`) a save made
  after loading a state reaches the card's folder (`mc0/BESCES-53931DT/`); before it, it stayed in memory;
- the mission list (News, "I need your help", "The Amurita Faith is your true home") and the mission scroll (four
  pages: the story, then type, destination, enemy, requirements and reward). Start accepts; "You have nothing
  equipped. Continue anyway?" takes Left, then Cross, for Yes;
- the second mission, a transport mission to Negishi Pavilion: "Mission Start", a new town level, the ninja with the
  letter's box on his back. The pause menu opens there.
The client's portrait on the scroll is drawn at half strength (vertex alpha 0x40) between the page and its text
(the text after it, as the game sends them): where the portrait is dark, dark text is hard to read, by design.
Save states for these tests: `build/hideout.state` (frame 15700, the Hideout's help), `build/scroll.state` and
`build/scroll2.state` (the mission scroll's pages 4 and 2).

The second mission completes too: its goal is not a character but an area. The mission's state (the object at
`gp-0x6b14`, `0x625c5c`) has a carry part at +0x440 (`0x2f7b10`: +4 the carrier, the receivers' list at +0x20) that
lists at +0x88 the goal's markers: objects with a world matrix (translation at +0xc0), here the four corners of a room
at (-71 to -112, 12.3, -450 to -473). Carrying the letter's box into it plays the contact's scene ("Good work, you have
done well."), "Mission Complete!", then the rating (three pages: the mission, the details with its rank, "Invisible"
for an unseen run, and the total with the trust gained); Triangle closes, back to the Hideout with the reward and four
new missions. The first mission's goal is in the same place: its markers are on the keep (147, 107, -322).
Leads that were not it: the script's `SearchSPCharacter` reads the mission's last killed character (+0x16c, set by
0x15e460, a character's death, role 1 of 0x11d410); `SearchFindCharacter` / `GetFindFlag` the characters who have
spotted the ninja (+0x114, from the guards' brains in state 5, `0x17d110`); the HUD compass does not point at the goal
(its star is fixed; an arm glows toward nearby enemies).
The Cheats tab's "To the mission's goal" (and `SHINO_TELEPORT_AT=<frame>:goal`) teleports to the markers' centre.

## The next four missions (2026-11-24)

From the Hideout after the second mission (`build/hideout2.state`), each of the four missions it offers plays to
"Mission Complete!", its rating and the Hideout:
- **Thievery** ("Get back what I loaned them", "My document": steal the documents and escape). The goal markers are
  the level's exits (three, far apart: reached without the documents, "Quit the mission and return to your
  hideout?"). The documents are a carriable item: the list the object at `gp-0x6ca8` gives (+0x3c / +0x40, each
  node's +8 an item, its translation at +0xc0). Beside it, Grab (Triangle) takes it; at an exit, "Mission
  Complete!" and "Send to the recipient right now?".
- **Total Destruction** ("The barbarians and the town", "Do it in secret": kill all enemies). "Remaining: N" counts
  down at each kill; the last plays a slow-motion kill camera. One-hit kills, then a teleport just behind each
  enemy and Square, kill them (unseen, from behind: the game's stealth kills).
What the cheats needed: the player's object at `gp-0x6a08` (the third mission's load leaves `gp-0x6a0c` pointing at
freed memory); a character's status block through the pointer at +0x45c (inside the object for the first guards,
+0x620; before it for others, -0x35d0, -0x7190: a scan of the object missed them, and "Do it in secret" kept three
enemies the cheats could not see).
Cheats tab: "To the goal" (the markers' centre when close together, else the nearest: an exit), "To the item to
take", "To the nearest enemy"; `SHINO_TELEPORT_AT` keywords `goal`, `item`, `enemy`; `SHINO_TRACE_ENEMIES=1`.

Played in one chain from there (each result kept: `build/hideout3.state` at 21450 after "Get back what I loaned
them", `build/hideout4.state` at 26900 after "The barbarian chieftain"): the Hideout then offers "Secretly eliminate
the enemy", "The barbarian chieftain" (an Assassination Mission, Phantom Forest: kill the barbarian chieftain), "Rub
out Itamiya, the merchant" and "Do it in secret"; all three kill missions play to "Mission Complete!" (99% for the
chieftain, 2,945 Mon). After the chieftain the story offers two **Reconnaissance Missions** ("First, observe Akame's
man", Sorgen Town; "Infiltrate Fudo Castle", Fudo Castle: "Get back alive", no failure condition, "Don't kill
anyone" as the special contract). Neither is finished yet: they have no goal markers and no item; the castle's lord
(type 0x15, 310 health, on the top floor at (-218, 368, -470)) and Akame's man (type 0x8f, pacing near (-305, 1020))
were reached unseen and watched for minutes, and the start point revisited, without an event. Seen by the lord's
group, the mission state records them (+0x3c0 their type, +0x3c4 a list, +0x41c the lord) and a fight starts in a
room the ninja cannot leave (a teleport out is pulled back). The scripts' events: `EventSPFind` (Cut_SPCharaStart),
`EventSPPullout`, `EventTimeup` (Cut_PlayerMorning), `Cut_PlayerReturn`. Both played without a fault.
`SHINO_TELEPORT_AT=<frame>:char<n>`: behind the character table's entry n (following one).

### How a mission ends (2026-11-24)

The mission's state (the object at `gp-0x6b14`) holds at +0x2b0 its mission (+0x50 the event byte the script's
`GetEventTiming` reads; `0x373c40` sets it, `0x373fd0` raises it with its rules) and at +0x440 the completion part
(+0 the mission data: +0x86 its type, +0x64 how many of it are needed; +0x16 / +0x17 met, +0x18, +0x19 flags). A
flow object (`0x101b4e0` here, `0x180470` its state machine at +0xc, 23 states) checks it each frame (`0x17ee10`):
+0x16 and +0x17 both set leads to state 12 (the mission over: "Mission Complete!", or "Mission Failed!" when a failure
condition was met) or, for the missions with a delivery prompt, 15 ("Send to the recipient right now?"). Events seen: 24 the item delivered or carried out (from `0x374058` and
`0x31b414`), 19 the ninja found by the target's group (`0x31adb0`, from the guards' brains in state 5: they join the
find list +0x114), 21 the target dead, 14 the end of "Get those Bow Designs". The completion code by type (`0x377650`'s
table at `0x60f560`; the exit markers' `0x3777d0`) has no case for type 16, the reconnaissance missions: what completes
them is still not found. Quitting from the pause menu ("Quit") fails a mission ("Mission Failed!", 0%, -2% trust);
27 game minutes of waiting, being found and fighting, killing the lord and returning to the start do not end it.
`SHINO_TRACE_MISSION_EVENTS=1` prints each event and each change of the flow's state (native sites `0x373c54` and
`0x180484`, the same stores when the trace is off: the boot's checksums are unchanged).

Cheats: `<frame>:done` in `SHINO_TELEPORT_AT` (the tab's "Complete the mission") sets +0x16 and +0x17: the game plays
"Mission Complete!" and its rating (used for "Infiltrate Fudo Castle": 90%, "Invisible"). The item teleport puts the
ninja just short of the item along the way he faces, so Grab takes it.

With these, from the Hideout after "Infiltrate Fudo Castle" (`build/hideout5.state`, frame 29900: "Steal the food and
weaken the enemy", "Steal back the jewel", "Deliver the Bow Designs", "Get those Bow Designs"): "Deliver the Bow
Designs" (the goal teleport) and "Get those Bow Designs" (item, then goal) play to "Mission Complete!". The two
"Steal" missions (type 10) are not done: their item is where the teleport cannot reach it (inside a building), and at
the exit the game asks to quit.

Further along the chain (`build/hideout6.state` at 33750 after "Deliver the Bow Designs": 100%, 3,775 Mon): the
Hideout announces "I feel the presence of enemies in my garden... I must defend my home." To Garden, Defend ("I hear
enemy footsteps in the garden... Shall I have some fun?") starts a garden defense: barbarians come in ("Remaining:
12"), "A barbarian has fled!" when one escapes; killing them all gives "Mission Complete!" and the news pages. From
there (`build/hideout7.state` at 37800) the Hideout offers "Akashiya, the Merchant of Death" (Assassination: 99%),
"Infiltrate Fudo Castle" again (78 Mon), "Honcho Shimoyashiki is in danger" (Total Destruction: 99%) and "I have no
more use for Akashiya, the merchant" (Assassination for another lord): played with the kill loop, which kills every
character, that one ends in "Mission Failed!" (0%, "What are you doing? Are you on the same side as Akame??"), the
game's own failure (event 30), not the port's.

Next in the chain (`build/hideout8.state` at 46100, after "Akashiya, the Merchant of Death": 4,707 Mon): "Defeat
Kagekatsu Nikko, Akame's samurai general" (Assassination, Kongo Pass: 99%), "Protect Kagekatsu Nikko, the samurai
general" (Guard: left alone in a battle on a ship's deck, the general dies, "Mission Failed!", the game's outcome),
"The stone that Akame is hiding" (Thievery, Fudo Castle: the chest taken and carried out, "Mission Complete!", then a
story scene, the flow's state 19: Akame and the stone's light) and "Get the mercenary list" (Thievery: 100%). The
item teleport now turns the ninja toward the item (his world matrix's rows: +0x10 right, +0x20 up, +0x30 forward) a
little short of it, on its floor: one Grab (Triangle) takes it; a second press drops it. "Steal the food and weaken
the enemy" and "Steal back the jewel" (type 10) are still not done: nothing to take where their item's position is.

Then: "The stone that Akame is hiding" carried out, "Send to the recipient right now?", a story scene (Akame, the
stone's light), 100% "Invisible" (`build/hideout9.state` at 56400, 4,942 Mon; the rating's last page waits for
Triangle). Offered: "First, observe Sadama's man" and "Observe Mokuami Fortress" (reconnaissance), "Get the mercenary
list" again (100%, then a story scene introducing the Mosu ninja clan's Usuba), "Kill Kagekatsu Nikko, the samurai
general" (Assassination: "Mission Complete!", the Mosu scene). "First, observe Sadama's man" completed by the cheat
(90%, the Mosu clan's Ageha) leads to `build/hideout10.state` (61120, 5,060 Mon; saved with the Missions submenu
open): "Now kill them in secret" (Total Destruction: "Mission Complete!", then the Kenobi ninja clan's Hebitonbo),
"Deliver the Secret Documents" (Transport: complete), "Get those Secret Documents" (Robbery: take them from a target
who carries them; the teleports do not, "Mission Failed!" and a scene: the game's outcome) and "Collect Grass from
Fudo Castle" (not done: nothing taken where its item is). Every scene, load and rating ran without a port fault.

## Playing the story with the cheats (2026-11-24)

`tools/play/shino_cycle.sh <state> <frame> <k> <out state> [done|kill|garden]` plays one day from a Hideout save
state: back to the Hideout's top menu (Cross, then Triangle three times: a dialog such as the garden raid's
dismissed), Mission Scroll, Missions, the k-th mission accepted (Start, then Left and Cross for "You have nothing
equipped"), completed with the `done` cheat (or the kill loop), then Cross and Triangle in turn through the rating,
the scenes and the prompts ("Send to the recipient right now?") back to the Hideout, where it saves the next state.
`garden` defends the garden instead (To Garden, Defend, the kill loop): when the Hideout says "I feel the presence
of enemies in my garden", the mission list cannot be opened until it is done. `tools/play/shino_chain.sh` repeats it
(k rotating 1 to 4 with k 0), a garden defense after any day that ended nothing; captures in `build/cyc/h<n>/`,
contact sheets with `tools/play/shino_review.py`.
From `build/hideout10.state`, days h11 to h36 played this way: every mission completed ("Set a trap with a secret
document", "Too many barbarians", "Finally, the samurai general's head", "Deliver the Rapid-fire Bow Designs",
"Defeat Yoshizo Iiyama, Sadame's samurai general", "Attack Sadame's starving troops", merchants and bear hunts...),
the Kenobi ninja clan's scenes, three garden defenses; 5,060 to 22,791 Mon; no fault.

Then on to day ~125 (`build/h230.state`, frame 1,650,550, 101,132 Mon): about 200 days of the chain in all, every
level, scene, rating and load without a fault. `tools/play/shino_pick.sh` chooses each day's mission from memory: the
offers are the static table at `0x6271dc` (0xa4 a mission: +0 the reward, -0x14 the client, -0x10 1 or 2 while
offered, 2 for a marked one; the offers first, in the list's order) and the titles are UTF-16 after "News" in the
list's text. It takes a marked mission, else a lord's own death (Kill / Defeat / Assassinate a lord), else the best
paid; the `mixed` mode runs the kill loop before the completion cheat, so a target really dies (a cheat-completed
assassination leaves him alive: Ichijo was "assassinated" twice, the second time for real, "I've done what I had to
do"). Along the way: Sadame dead ("Death" on his status page), Ichijo assassinated, trust at 0% with both lords and
Akame's at -25% ("foolish enough to betray me"), the Mosu and Kenobi clans' scenes, Akame's betrayal scene ("Crow
dared to betray me?! Cut off his head!"), which plays when its letter is opened: the story's scenes are in the
Letters (Mission Scroll, Letters), which the chain never opens. The game's ending was not reached: by day 94 (Check
Status) a Let's Play's story had ended at about day 89, so the path this chain took (random lords served and
betrayed, objectives met by cheats) has left the main story somewhere it does not finish, or its next step is in a
letter or a choice the driver does not make. Still to understand: what the story's last missions need.

### What moves the story: soul fragments (2026-11-25)

The story advances by soul fragments, eight of them (Etc shelf: "Soul Fragment 1".."8"); Check Status's hint is the
stage (the game's Condition0..11 texts: stage 2, "What could have happened in my past...?", holds until the third
fragment). The lords help find them only with respect ("The lords will only help you with information about the soul
fragments if they have respect for you"; Onji: "just pick one and work hard for them"). The ~200-day chain above stayed
at stage 2 for good: it served lords at random.
- Respect: Ichijo's, Akame's, Sadame's percent at `0x6264e8`, `0x6265a8`, `0x626668` (int). The day's offers: +0x1c
  the client, +0x20 the target (0 Ichijo, 1 Akame, 2 Sadame, other: none).
- Replay serving Ichijo from day 9 (`build/i11.state`; `LORD=0 LETTERS=1 PREFIX=i PICK_MODE=auto shino_chain.sh`):
  Ichijo at 100% by day 30, his letters and scenes ("Rumours of a surviving Asuka Ninja", a bow, the first special
  move), Sadame and Akame's men dying; then "The whereabouts of one of the shining stones" letters (the Mosu, the
  Kenobi, barbarians have one).
- Rival ninja: after some missions "I sense danger... This might be the end for me... Continue anyway?" (the flow's
  states 13, then 14 while shown; the cursor on No). `SHINO_ANSWER_DANGER=1` (the runner's pad assist) answers Yes: a
  Mosu scene ("I'm not laughing, Usuba.", "Leave! I have no reason to kill you.", "We know what you are!"). The
  "A chance to dispose of the Mosu ninja" missions kill Mosu (120 health) without giving a fragment.
- The third fragment's mission, "A glowing stone in Dandala Peak" (Ichijo, 73 Mon, type 16): a barbarian chief (320
  health) on a cliff top at (210, 468, 298) guards a chest with a purple glow (`item/Itm_kakera_01`; the scripts'
  `DropKakera`). Beside it, facing it, Triangle lifts it (the mission's carrier, +0x440 +4, is then the ninja). The
  exit is the tunnel at the start (-24, -17, -11): walking in ends the mission ("Return to your hideout?", event 24,
  "Mission Complete!") with or without the chest, and gives no fragment; the mission is offered again. A teleport
  leaves the chest behind (the carry stays set, its model does not move); a walk (`<frame>:walk<x>,<z>[,<step>]` in
  `SHINO_TELEPORT_AT`, the ninja moved a little each frame, his height left to the game) carries it down the first
  slope but drops it at the next ledge. Not found yet: how the chest is meant to reach the exit (or open), and where
  the chest object keeps its position. The Let's Play's "Do we mail the stone to Sadame, or do we grab it right
  away?" suggests a delivery prompt that keeps the fragment.
- Solved (a GameFAQs walkthrough, serenitychan's, "Key Mission Titles To Look For", and the lords' own letters: "The
  stone should be stored in a sturdy wooden box. You won't be able to open it, so just send it on to me"): the chest
  must leave the mission carried. Falls drop it (a teleport, a walk off a ledge, a fly), and the mission's carrier
  (+0x440 +4) stays set after a drop, so it says nothing. The chest's physics keeps its position in 27 places (its
  matrices, 0x1a6ea20 / 0x1a6ea60 in that save, and the body's, 0x1a75xxx-0x1a77xxx): written all together for 30
  frames (`SHINO_POKE_AT`, the offset applied to each), the chest stays where it is put. Put beside the ninja at the
  tunnel (-24, -17, -11), lifted (Triangle) and carried in (Up): "Return to your hideout?", "Mission Complete!", the
  fragment's memory scenes, "第二章 忍びの道 人の道" (chapter 2), Soul Fragment 3 on the Etc shelf, the status hint
  "A traitor.....?! They tricked me!" (day 34, `build/frag3_ok.state`).

### The ending (2026-11-26)

Played to the ending on the port, serving Ichijo from day 9 (`tools/play`, `LORD=0 LETTERS=1 PICK_MODE=auto`), every
story mission in the walkthrough's order, 110 days in all (days are approximate):

| Day | Mission | What it took |
| --- | --- | --- |
| 34 | A glowing stone in Dandala Peak | the chest moved to the exit by hand (above): fragment 3 |
| 40 | Find the glowing stone (Onji's) | the Kabuto fight (the cheats find the player by his status block there): fragment 4 |
| 59 | The stone that Akame is hiding | the chest carried out (flat ground, the teleport keeps it): fragment 5 |
| 63 | A threatening letter | Kinu rescued from the Kenobi duo (a longer day for the scenes): fragment 6 |
| 86 | The stone that Akame is hiding (again) | the same: fragment 7 |
| 94 | The killer whale's eye | Gamuran's scenes; Goh's soul put in a bear |
| 96 | Find your body | walked through as the bear; Goh human again |
| 109 | (Akame dies) | "The demise of Kagetora Akame"; "Barbarian hunting" and "The final enemy" left |
| 110 | The final enemy | Kabuto, then Gamuran (1,450 health): the ending, Goh and Kinu's epilogue, the credits |

Every scene, movie, boss fight and the credits played without a port fault. What the drivers needed on the way: the
picker reads the list's rows in screen order (each title's widget holds its y; only the rows on screen exist, so a
dump per scroll step), two-line titles, story titles first; the rival ninja's "I sense danger..." answered Yes;
`SHINO_KILL_SPARE` (leaders spared while others live, grabbed where they fall); no Start in the cycle's prompt spam
(in play it opens "Abandon the mission?", and abandoning the final mission is a Game Over: the first try went to the
title screen and a new game).

## The 3D's resolution and widescreen, mapped (2026-11-27)

**Resolution.** The GS translator's triangles are drawn by the GPU into a target of the window's picture size times
BozoEngine's render scale (`present_gl.cpp`): the 3D is already at the window's resolution, not the console's 512 or
640 wide. A render scale of 0.25 makes it blocky, 3 smooths its edges (checked with a temporary switch). The port does
not use the engine's video settings (`WindowOptions::videoSettings` off): no `video.ini`, no Video tab, no render
scale, anti-aliasing, texture upscale or smoothing, window modes. Turning them on is the way to a sharper 3D, not a
native renderer.

**The camera.** Every 3D renderer (five functions at `0x474e70`, `0x479f10`, `0x47b1f0`, `0x491ca0`, `0x4931d0`) uploads
the same block with `UNPACK V4-32`, 10 quadwords to VU1 address 1008 (`0x6c0a03f0`), from double-buffered packets
(`0xdb6e00`, `0xe36200` in play): 1008-1011 the view-projection matrix (lane 0 of each row the x output, lane 3 the w),
1014 lighting constants, 1015 the screen scale (512, 512, 65548), 1016 a depth offset, 1017 the GS centre (2048, 2048).
The setup program at pair 0 maps x through `x - 0.5 w` (its `loi 0.5`), then the scale and the centre: the screen's
middle is x = 0.5 w. libvu0's `sceVu0ViewScreenMatrix` (`0x2c5058`) and `sceVu0CameraMatrix` are linked but not
called; the renderers read the current camera through `0x672df0` (cleared by the frame's end).

**Widescreen, tried.** Each row's x output squeezed about the middle, `x' = (x - 0.5 w) * 0.75 + 0.5 w`, at that
UNPACK (a temporary switch), the picture then shown 4:3 wider
([docs/screenshots/2026-11-27-widescreen-test.png](screenshots/2026-11-27-widescreen-test.png): 4:3, then the wider
view stretched to 16:9): more of the scene at both sides, centred, the buildings there drawn. Left to do for a
setting: the HUD (2D programs, stretched with the picture: to be squeezed back where the translator draws them), the
compass needle (drawn with the 3D camera: it leaves its 2D ring), floor pieces missing at the bottom corners (the
game's culling at the 4:3 frustum: its planes or VU0 box tests to widen), and the picture shown at 16:9
(`setPictureAspect`).

## Next

- Speed: the castle's load takes about 15.5 ms of CPU a frame (40 ms at 25 frames a second). The compiled VU1 code is
  the largest part (about 1.5 ns a pair: the arithmetic and its clamping). Then the primitives' assembly
  (`packedFast`, about 6%). In the busiest play scenes the recompiled EE code ("other" in `SHINO_PROFILE`) is now the
  largest bucket. VU0 is compiled and the movies are no longer decode-bound (2026-11-26). The busiest fight replayed
  holds 52 game frames a second uncapped or more, and takes a quarter less CPU since (VU0's registers in place, MMI in
  SSE2: "A busy fight, measured"). Next there: the compiled VU1 code (35%).
- 3D: native draws at the game's model functions (the bring-up path draws the GS packets).
- Sound: listened to through the boot, the intro, the story's movies, the castle's cutscene and play (2026-11-27, about
  4 minutes in the window, 8,683 frames at 25 a second): nothing heard wrong.

## PAL pacing

The game resets the GS with video mode 0 (`sceGsResetGraph`): the console's own standard. A PAL console's is PAL, so
the port sets BozoEngine's `palConsole` (fields at 50 Hz for mode 0, as for mode 3).

The game flips every second field: its vblank handler `0x442fb0` counts vblanks up to a minimum of 2 (`gp-0x6850`)
before it flips, at the title and in play alike (`SHINO_TRACE_VBLANK=<flip>` prints the count). On the console it runs
at 25 frames a second; a host frame (one flip) spans two fields of the game's time (11.8M EE cycles). The window used to
hold the game to 50 frames a second, which ran it at twice its speed wherever the port kept up (the boot, the menus).
It now holds each frame to the fields it spanned (`runtime/shino_main.cpp`, the cap 50 / fields: 25 in this game);
`PS2X_FRAME_CAP` overrides it.

### 50 and 60 frames a second (2026-11-27)

The game's logic follows its own frame timer, not the frame count: its struct (pointer at `gp-0x6cec`, 0x1079df0 in
play) holds +0 the vblanks a second (50.0, hard-coded by its constructor `0x133df0`), +4 a vblank's seconds (1/+0),
+8 the frame's step in seconds, +0xc the frames a second, +0x1c the vblank clock (`gp-0x6ce8`, a second vblank
handler `0x133dd0` adds 1 a field), +0x20-0x3c the last 8 frames' vblanks and +0x44 the largest step (0.06). Each
frame `0x134010` averages the 8 and multiplies by +4. Measured:
- `SHINO_FRAME_VBLANKS=1` (the frame's minimum forced to 1: a flip every field, 50 a second): the ninja's walk
  1.35 units a frame becomes 0.67, the same distance in the same time; the game keeps up within the field.
- `SHINO_60FPS=1` (experimental): fields at NTSC's rate (`palConsole` off; a save state's rate set again), a flip every
  field, the timer's +0 / +4 written as 60 and 1/60 when its constructor has set 50, frames held to 60 a second.
  The walk 0.563 a frame (1.348 x 25/60 expected); after 10 s of game time the ten guards of the frame-5200 state
  within 5 units of where they are at 25 (patrols, AI on the timer); 60.0 frames a second in the window in play.
To check before it is a setting: what may count frames (menus, the HUD, effects, the in-engine cutscenes), the other
hard-coded 0.02s (`0x174910`, ...), the movies (PAL mode 3: 50 Hz while they play), the sound, and the busiest scenes
(the fight's day: 54 to 60 in the window).

**Gravity at 60 (2026-11-27).** `SHINO_TRACE_TIME=1` prints each frame the game's clock (the timer's vblank count, +0x1c)
and step and the ninja's position, so play at 25 and at 60 compares by game time. A standing jump: walking, guards and
the rise matched, but the jump went 15.83 high and stayed up 0.70 s at 60 against 14.14 and 0.60 s at 25. The ninja's
vertical velocity (+0x314) is stepped by `0x110140` (the other characters' by `0x3a25a0`, its law `0x10d970`):
v += g(v) h, g -450, or -100 - 0.4 |v| while |v| is 15 or less (the top of a jump), a fifth of that gliding; his height
by the new v h after it (semi-implicit). Its result depends on the step: at the console's 0.04 his height runs straight
from frame to frame, the slope of each 0.04 s that step's velocity, chosen at its start (overshooting the |v| = 15
boundary by a step). Native sites at the five velocity stores (`0x11032c`, `0x1104fc`, `0x1105b0`, `0x11095c`,
`0x3a2640`; `runtime/shino_port.cpp` `gravityStep`) keep, for each character, a shadow of that velocity stepping at
0.04 s in the frames' own time, and store each frame the velocity that moves him as far as the console would in that
frame's time: his height is the console's path, sampled 60 times a second. A velocity the game's other code wrote since
(the take-off's, a landing's) restarts the shadow from it. Only when the port runs another rate than the console's
(`SHINO_60FPS`, `SHINO_FRAME_VBLANKS=1`). The standing jump at 60 now follows the 25 path within 0.008 (peak 14.13); a
running jump's rise matches too, and its catch on the railing lands a frame of 60 earlier (collision is tested each
frame: at 25 the ninja overshoots before it is seen).

**The rest of the audit (2026-11-27)**, by game time (emulated cycles or the timer's clock), 25 against 60:
- the game's RAM over 8 s of idle play: of 44,399 floats that moved, 25,483 moved exactly alike, the 2.4 times faster
  ones a scratch buffer of transformed vertices rebuilt each frame; the frame counters and buffer pointers that do run
  by frames are bookkeeping;
- an attack: its state (+0x2e4 = 203) lasts 0.88 s at 25, 0.90 s at 60, its other phases alike within a frame;
- the movies: the display goes to PAL's 50 Hz while they play, at either rate; their audio feed sends the same bytes a
  vblank (2,936 against 2,863), and the boot's sound recorded at both rates correlates at 0.999, 0.5 s earlier at 60:
  the logos before the movie are that much shorter (counted partly in frames);
- the final fight in the window at 60: 60.0 frames a second (8-11 ms of CPU a frame); headless 98 to 123.
Left: the logos' half second; a save state made at 25 loaded at 60 moves the timer's clock once (its rate set again).
