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
TOPS double buffer) and runs the programs (pair semantics: the lower reads before the upper commits; Q's latency; the
MAC flags 4 pairs late; branch delay slots; the E bit; vf0 the constant (0, 0, 0, 1)). XGKICK's packets (PATH1),
DIRECT's (PATH2) and the GIF channel's (PATH3) go to `runtime/shino_gs.cpp` / `shino_gs_state.cpp`: the GS registers
are kept, primitives become `Renderer::drawOverlay` triangles in window pixels (each vertex less its context's
XYOFFSET), and the textures are decoded from a copy of the GS local memory the image transfers fill (the GS's block and
column arrangements, checked as permutations at start-up). Only the draws into the displayed frame buffer are shown.

This is a bring-up path, not the house style: the other ports answer the game's own draw functions natively with
data converted offline. It gets the menus on screen and gives a reference picture to replace it piece by piece.

Debug switches: `SHINO_TRACE_LIST` (the command list), `SHINO_TRACE_FLIP`, `SHINO_DUMP_PRIMS=<flip>`,
`SHINO_DUMP_KICKS=<flip>`, `SHINO_TRACE_PATH1=<flip>`, `SHINO_TRACE_VIF=<flip>`, `SHINO_TRACE_KICK=<flip>`,
`SHINO_DUMP_MICRO=<flip>` (VU1 memories at each program start, for `tools/shino_vu1_disasm.py`),
`SHINO_TRACE_PROGRAM=<flip>:<n>` (one program stepped pair by pair), `SHINO_TRACE_DRAWS`, `SHINO_TEST_QUAD`.
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

`SHINO_SKIP_MOVIES=1` answers `0x327130` as a finished playback (each movie skipped); `SHINO_TRACE_MOVIE=1` prints
the sound feed (`0x672540`: its EE ring bytes, the IOP buffer's write position, the bytes sent) every 25 fields.

## Next

- Sound: the sound effects and voices in play; the Acquire logo's short sting is silent.
- "New Game": the game's 3D, through the same VU1 path first, then native draws at the game's model functions.

## PAL pacing

The game resets the GS with video mode 0 (`sceGsResetGraph`): the console's own standard. A PAL console's is PAL, so
the port sets BozoEngine's `palConsole` (fields at 50 Hz for mode 0, as for mode 3), and the window paces the game
at 50 frames a second (`PS2X_FRAME_CAP` overrides it).
