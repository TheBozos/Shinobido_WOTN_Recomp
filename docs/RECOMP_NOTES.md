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
3. **Current stop**: `no code at 0xffffff` (frame 0, clock 904192) after 8 kernel calls and 358 library calls
   (353 `memset`, 2 `memcpy`, 2 `Deci2Call`, 1 `strlen`). The two `Deci2Call`s come from libkernel's TTY / DECI2
   wrappers (`0x2c6cc0`..`0x2c6ed0`, called from `0x2c7380`, `0x2c7138`, `0x2c6fa0`, `0x2c7478`). Main
   (`0x100ba0`) first calls `0x100870`, then `0x100c30` (the constructors), `0x100230`, `0x100260`, `0x100290`,
   `0x1002a0`, `0x108320`, `0x133df0`, `0x1087c0`. Next: find which call returns or jumps to `0xffffff` (a return
   address or function pointer that reads as all ones: a native answer the game reads differently, or a table not
   filled yet), with a temporary trace in the dispatcher.

## Next

- Boot: the `0xffffff` stop above, then the rest of the SDK's boot (`sceSifInitRpc`, the IOP reset and module loads,
  `sceCdInit` / `sceCdMmode`), naming each function as the runner reaches it (TTA's order: the kernel and SIF calls,
  the CD init, the IOP reset, the sound handshake, the GS path resets, the pads).
- Sound: Acquire's driver `TORAST.IRX` / `TORASE.IRX` (CodeWarrior IOP modules, "Ads_driver") with Sony's `LIBSD` and
  `SDRDRV`, statically compiled with BozoEngine's `tools/native_irx` (TTA's `tools/tta_bozo_audio.py`, Onimusha's
  `tools/oni_bozo_audio.py`); the `.TRS` archives are their data.
- Files: `VOLUME.DAT`'s directory (the game opens `volume.dat` and reads `chara/*.bin`, `weapon/wpdata.bin`... from
  it): how the game reads it (libcdvd streaming, `CDVDSTM.IRX`, or fileio), for the converters later.
- Movies: the PSS files through BozoEngine's native libmpeg (`BOZO_REQUIRE_FFMPEG`, TTA's `scripts/build_ffmpeg.sh`).
- Drawing: native hooks at the game's renderer functions, once the boot reaches its first frame (TTA's order: movies,
  then 2D/UI, then 3D).
- PAL: the runner paces 50 fields a second (`framePeriodCycles = 294912000 / 50`); check whether the game offers a
  60 Hz mode at boot (many 2006 PAL releases do) before tuning the pacing.
