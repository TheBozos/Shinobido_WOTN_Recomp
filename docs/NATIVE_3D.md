# Native 3D: the map and the plan (2026-11-27)

The port draws today through a translation layer: the game builds its frames as PS2 DMA chains, the port runs their
VIF codes and VU1 microprograms (recompiled) and turns the GS packets they kick into GPU triangles. BozoEngine's way
(docs/PLAN.md in the engine: "No DMA, VIF, GIF, GS, VU") is the game's draw calls answered by the native renderer from
the game's own data, the packets left unread and in the end not built. This file is the map of how Shinobido draws its
3D, and the plan to get there.

## How a 3D object reaches the screen

**Microcode sets.** Five VU1 microcode sets draw the 3D in play (`SHINO_VU_SURVEY=1`, 500 frames from the frame-5200
state; the pairs run):

| Set (EE address) | Programs | Pairs | Share |
|---|---|---|---|
| `0x539c40` (image 45) | setup at 0, vertices at `0x3968` | 550M | 66% |
| `0x579a10` | setup at 0, vertices at `0x3c38` | 247M | 30% |
| `0x599520` | setup at 0, vertices at `0x3a68` | 35M | 4% |
| `0x59d2a0` | setup at 0, `0x2f88` | 4M | |
| `0x5e7ce0` | one at 0 | 0.7M | |

Each is uploaded with MPG in 256-pair pieces before the objects that use it.

**The camera block.** Each renderer's first transfer is the same block, `UNPACK V4-32` of 10 quadwords to VU1 1008:
1008-1011 the view-projection (lane 0 of each row the x output, lane 3 the w), 1014 lighting constants, 1015 the
screen scale (512, 512, 65548), 1016 a depth offset, 1017 the GS centre (2048, 2048). The setup program maps x and y
through `v - 0.5 w`, then the scale and the centre. The renderers read the camera through `0x672df0`.

**An object.** In a frame's VIF1 chain (`SHINO_TRACE_DMA=<flip>`, `tools/shino_vifwalk.py`), each object is:
- a `CNT` with `DIRECT 6qw`: a GIF packet setting the material's GS registers (TEX0 and the rest, A+D);
- a `CNT` with `UNPACK V4-32` of 1, 1, 1 and 3 quadwords to VU1 1018-1023: the object's constants (its matrix and
  lighting, to be read in the setup program);
- `DIRECT 4qw`: the GIF tag the primitives will carry;
- a `CALL` into the model's own mesh chain in model memory (`0x0118_0000` up), `BASE 0`, `OFFSET 0xd7`;
- `MSCAL`: the vertex program.

**A mesh** is a VIF chain kept in model memory, five quadwords a vertex (`STCYCL 1, 5`), written at TOPS:

| Slot | UNPACK | What |
|---|---|---|
| 0 | V3-32 | position, object space (float) |
| 1 | V2-32 | texture coordinates (float) |
| 2 | V4-8 unsigned | colour |
| 3 | V3-8 signed | normal |
| 4 | V4-32 | not known yet (from a `REF` into model memory) |

`ITOP` gives the vertex count. Meshes are float vertex arrays in RAM: a native renderer can take them as they are,
or a converter can read them from the disc's `VOLUME.DAT`.

**Textures** are in model memory too, ready for the GS: `DIRECT` GIF packets with BITBLTBUF / TRXREG / TRXDIR and an
IMAGE transfer (4 and 8 bits a texel), the palette in an 11-quadword block beside it.

**The vertex program** (`0x3968` in set `0x539c40`) starts `jr vi15`: the setup program picks one of several
software-pipelined variants by the object's flags (`ilw vi2, 1023`: pairs 434 to 1438). Each loads a vertex, transforms
it by the object's rows (vf6-vf11), divides by w, makes fog and the lit colour (vf20-vf22 against the normal), clip
tests, and writes STQ / RGBAQ / XYZF into a GS packet it kicks.

## The plan

1. **Read what each vertex program computes**, variant by variant, from the recompiled code and the interpreter
   (`SHINO_VU_VERIFY` gives inputs and outputs to check against): the transform, the lighting, fog, slot 4, the
   primitive type and strips, clipping. Start with set `0x539c40` (66% of the 3D).
2. **Draw those objects natively** at the program's start (`MSCAL`): the mesh's vertices from VU1 memory, the
   object's matrix and lights, the material from its GIF packet, given to `Renderer::draw` (object space, a matrix to
   PS2 screen space); the program not run. The GS path stays for everything else, and `SHINO_NATIVE_3D=0` turns it
   back on, so each object can be compared picture by picture (`SHINO_CHECKSUM`, captures).
3. **The other sets**, the same way (`0x579a10` next: 30%).
4. **Up to the game's functions.** Find the EE functions that build an object's unit (the five renderers that send the
   camera block: `0x474e70`, `0x479f10`, `0x47b1f0`, `0x491ca0`, `0x4931d0`) and the model structures they read; draw
   from there and stop building the packets. Meshes and textures read once per model (or converted offline into the
   engine's mesh and texture packs: `bozo/render/mesh_pack.h`, `texture_pack.h`).
5. **2D, particles and effects**: the HUD, menus, text, the afterimage and the screen effects, natively.
6. **What it opens**: resolution and anti-aliasing (the engine's video settings), widescreen from the camera (not a
   squeezed picture), 60 frames a second from the game's own timer (RECOMP_NOTES.md), texture packs.
