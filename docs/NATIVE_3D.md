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

## The main renderer (set `0x539c40`), read so far

**Which variants run** (600 frames of play from the frame-5200 state, 1023.x at each program start): the vertex
program `0x3968` ran 100,512 times with flags `0xb`, 4,648 with `0xa`, 537 with `0xf`; the setup picks the same entry
(pair 1023, `isw.y vi12, 1023`) for `0xa` and `0xb`, so one variant draws 95% of this set. (The second set's
`0x3c38`: 78,191 with flags `0x9`, 3,808 with `0xb`.)

**An object's constants** (VU1 memory at the setup's start):
- 976-979: the object's world matrix (rotation rows, then the translation; w 1);
- 980 on: the light list (below); 980.w 0 is none;
- 1018: the GIF tag the vertices go out with, PRIM `0x5c` (triangle strips, Gouraud, textured, alpha-blended; PRE),
  registers ST, RGBAQ, XYZF2;
- 1019: the material colour (about 1.0, alpha 0.5);
- 1020: 255 (the colour clamp);
- 1021-1022: fog (261, 0.0038, -2.0, 2500 / 261, 0.0038, 1.0, 1);
- 1023.x: the flags (bit 1 lit, bit 3 ...), 1023.y the variant's entry the setup writes.

**The setup program** first unpacks the vertex buffer at TOPS (colours and normals from bytes: `itof0`, x 1/128),
blends two vertex sets where the object morphs (`mulaw / maddx` with vf27's weights), then walks the light list:
`ilwr.w vi3, (vi2)` is a light's type, its next two quadwords a direction or position and a colour. Types 1, 2, 128,
129 and 130 have their own loops (an ambient term added to every vertex; a directional N.L with the vertex normal;
a point light from its position; ...); type 0 ends the list: every vertex colour clamped to 255 and multiplied by the
material colour (1019). The colours are per vertex, made before the vertex program runs: a native renderer can make
them on the CPU the same way.

Without lights (`0x0c30`) each colour is the vertex colour times the material colour, after the morph if any. The
object's world matrix (976-979) only takes the lights into the object's space: the matrix at 1008-1011 is the whole
model-view-projection, sent with each object (the ten copies of that block found in RAM, one an object). The setup
ends `jr vi12` into the variant (`0x0d90` is entry 434's: four quadwords a vertex, position, UV with depth in z,
colour; the camera rows vf28-vf31, the divide by w, `ftoi4` XYZ and `ftoi0` RGBA, the triangle's normal by
`opmula / opmsub` for culling).

So at an object's vertex program everything a native draw needs is in VU1 memory: the positions (morphed), the lit
colours, the UVs, the MVP, the screen mapping (1015, 1017), fog (1021-1022), the vertex GIF tag (1018) and the
material sent before it.

Still to settle: this lighting loop steps four quadwords a vertex, the mesh decoded above five (that one is likely the
second set's, the characters'); the vertex variant at pair 1023 itself (transform by 976-979 and the camera, fog,
clipping, strips and their restarts).

**The screen mapping.** The setup zeroes rows 0-2's z and sets row 3's z to 1, takes 0.5 w off x and y, scales by
1015 and adds 1016 + 1017: with (x, y, w) the MVP (1008-1011, its quadwords the matrix's columns: x row0 + y row1 +
z row2 + row3) applied to the position,
X = 2048 + 512 (x/w - 0.5), Y = 2048 + 512 (y/w - 0.5), Z = -19.67 + 65548 / w. The UVs are slot 1's s and t (STQ =
s/w, t/w, 1/w), the colour slot 2's after the lighting (`ftoi0`).

**Batches.** An object's first batch is `MSCAL 0` (the setup, then its variant); each next one `MSCNT`, which starts
where the variant ended (its E bit's pair is the one before `0x3968`, so TPC is `0x3968`; elsewhere in this file
"MSCAL 0x3968" means that), whose
`jr vi15` goes back to the batch's prelude at pair 51 (`xtop vi1`, `xitop vi9`: the batch's buffer and count; the
lighting; `jr vi12` into the variant). The variant ends `vi15 = 51` and the E bit.

## Stage 1, as built (2026-11-27)

`SHINO_NATIVE_3D=1`: the batches of set `0x539c40` whose variant is the main one (pair 1023) drawn natively
(`runtime/shino_native3d.cpp`). The VU program runs its setup and lighting (compiled) and stops at the variant's entry
(`Unit::stopAt`: the VU recompiler's computed jumps check it); the batch is read from VU1 memory and given to the GS
translator's lists as a native batch (`gs::nativeTriangles`: object-space triangles and their matrix to window pixels,
XYOFFSET folded in, in the frame's order with the other draws, the material and texture of the GS registers and the
vertex GIF tag's PRIM): positions, lit colours, UVs, the MVP times the screen mapping, fog per vertex (the GS path
does not draw fog yet: off on both), each triangle's ADC as the variant computes it, and the clipper's decisions:
- all three corners nearer than the near plane (w < 1) or farther than the far one (w > 2500): rejected;
- all three outside one guard-band plane, every corner in front: rejected (behind the camera a corner's x and y are
  mirrored, so the clipper cuts at the near plane first);
- across the near or the far plane: cut there (Sutherland-Hodgman in clip space, the attributes interpolated);
- the guard band's cuts left to the GPU.
Then the variant runs on (compiled) with its GS packets not sent: what it leaves in VU1 memory (the clip positions in
slot 0, the ADC in slot 1's w) and in the registers is read by later programs, the lit objects' passes among them
(without it, the sunlit top of a cliff in the final mission came out dark).

Checks:
- `SHINO_NATIVE_3D=verify`: each batch made natively, then by the VU, the clip positions and ADC flags compared:
  195,109 batches (9.0M vertices) of play from the frame-5200 state and 277,590 of the final fight, none different.
- Frame-exact window captures (`SHINO_REAL_TIME=0 PS2X_FRAME_CAP=25`, so the window presents each game frame;
  without the cap a capture can land a frame late): 0.16 to 1% of the pixels differ by more than 8 of 255, at edges
  (float vertices against the GS's 1/16 of a pixel) and in texture sampling.
- A whole mission day (23,000 frames, 7.1M batches) natively, headless: no fault; the game's RAM at frame 5450 is the
  same as without (nothing the game computes depends on how it is drawn).

Verify also reads the GS packets the variant's fast path sends (`captureKick`: each vertex's ST, RGBAQ, XYZF2) and
compares them with the native batch's, field by field. On the frame-5200 play, 98.75% of 694,461 vertices agree;
the others are at strip restarts: where the batch repeats a vertex twice, the VU's packet has it three times, one of
them mixing vertex 4's position with a later vertex's ST and Q, and the batch's last vertices shifted out. The
first pass over the same buffer matched exactly, so this comes from the output loop's software pipeline (loads,
DIV's 7-cycle Q and stores overlapped across vertices): either the port's VU timing or the console itself does it.
It touches degenerate triangles and a batch's last two; which side is right needs a reference from a console.

`SHINO_NATIVE_3D=skip` leaves the variant out (its first pass's memory results written back): the registers it would
leave are then missing, so lit passes come out wrong; it is there for finding what later programs read.

## Stage 1b: the characters (2026-11-27)

The characters' renderer (set `0x579a00`, image 67, 30% of the 3D) is the same template: the same setup code (the
camera rows, the flags' choice of variant, the batch's prelude at pair 51 and its lighting), its own output buffers
(430 and 565), and a skinning pass: the batch comes in at five quadwords a vertex (position, UV, colour, normal and a
fifth, the bones' weights) and its variant reads four (position, UV, colour) at `vi1`. Its main variant (pair 644,
flags `0x9`: 78,191 of 81,999 runs of `MSCAL 0x3c38`) has no clipping and no first pass written back: each vertex's
clip position by the camera rows (w scaled by kz = 1014.z, Q = kz / that), the same screen mapping and fog as the main
renderer, and a facing test in screen space: P = the clip position times Q, the z of `A.x B.y - B.x A.y` with, on odd
vertices, A = P(k) - P(k-1) and B = P(k-2) - P(k-1), on even ones A = P(k-1) - P(k-2) and B = P(k-1) - P(k), ADC
where its sign differs from the cull mode (1023.z), carried by 2048.5 added to the fog word.

`runtime/shino_native3d.cpp` takes it over the same way (a table of kinds: the microcode, the variant's entry, the next
batches' program, what the variant does with a strip). Verify reads its packets too: on the frame-5200 play 2.57M
packet vertices and on the final fight 9.67M, 99.9% identical in every field (the rest the strip restarts' pipeline
effect); frame-exact captures with both renderers native differ at about 1.2% of the pixels (edges); a mission day
(12.6M batches) without a fault.

## Stage 1c: the stop set and the remaining variants (2026-11-27)

The programs stop at any of a renderer's native variants (`Unit::stopPairs`, a table of pairs the compiled code's
computed jumps check: the setup chooses the variant only as it runs). The main renderer's variant 554 is the
characters' 644, instruction for instruction: drawn natively the same way (130,000 more batches in the final fight,
the packets' differences unchanged).

What is left, by runs (the final fight and the frame-5200 play, `MSCAL` starts): the main renderer's variant 1438
(about 28,000: another first pass and clipper), the characters' 1113 (10,000), set `0x599510`'s variant 657
(44,000 in the fight), set `0x5e7cd0`'s 1044 (10,000) and set `0x59d290`'s 520 (a few thousand). 657 draws each
strip twice: an A+D packet (FOGCOL, TEX0_1) and the strip, then another (FOGCOL 0, TEX0_2) and the strip with a
second UV set and colour the setup leaves in a buffer (`vi12`, three quadwords a vertex) and the positions the variant
writes there. Natively it needs the variant's kicks replayed in order (the register packets applied, the strips drawn
from the batch's own positions): the native mode now drops every kick of a taken-over variant, register packets too,
so 657 stays on the GS path for now.

## Stage 1d: replayed variants (2026-11-27)

The variants not read yet are replayed: the program stops at the variant as for the others, the batch's positions,
screen points and matrix are kept, the variant runs with its kicks held and each GIF packet it sends is kept whole, in
order (`captureKick`); then (`finish`) the packets go out in that order: a PACKED strip of the batch's length whose
vertices land on the batch's own screen points (within half a pixel) is drawn natively from the batch's positions with
the packet's ST / Q, RGBA, fog and ADC; any other packet (A+D registers, a clipper's output, a strip that does not
line up) goes to the GS translator as it is. A wrong guess about a variant's layout can only send its packets the old
way. Replayed: set `0x599510`'s two-pass 657 (FOGCOL / TEX0_1, then FOGCOL / TEX0_2 with a second UV set), the main
renderer's 1438, the characters' 1113, set `0x59d290`'s 520 and set `0x5e7cd0`'s 1044.

On a mission day (22.3M native batches): 718,714 batches replayed, 1.32M strips, 98.5% drawn natively, the rest the
old way; no fault. Frame-exact captures of the town and the final fight: 0.9 to 1.2% of the pixels differ, at edges.
The replayed strips' attributes still come from the VU's packets: drawing them from the batch too (as for the main
variants) is the next step for each.

## Stage 2a: the main renderer's setup, read (2026-11-27)

What the main renderer's setup leaves in a batch (four quadwords a vertex: position, UV, colour, normal) is now
computed natively from the batch as it arrives (`nativeSetup`, checked in verify mode; the VU still runs it):

- the morph: when 1020.w (the weight a) is not 0, the position and the normal blend `p (1 - a) + target a` towards the
  targets stored after the batch (positions at TOP + 4 count, normals after those);
- the colour is slot 2 converted (`itof0`), the normal slot 3 over 128; unlit (980's type 0) the colour times the
  material colour (1019) and that is all;
- the light list from 980 (the type in .w; 976-979 bring each light's position into the object's space): type 2 a
  constant colour (times 1020.x) added; type 128 a point light (colour times 1020.z, radius rec1.w times 976.w): each
  vertex gains colour x max(n.d, 0) x (1/|d| - 1/radius) where that is positive; type 0 ends the list; then the colours
  clamped to 255 and multiplied by the material colour. Types 1 (directional, 0x598), 129 (a spotlight gated on MAC
  flags, 0x830) and 130 (0xa18) are not read: such a batch reports "a light not read".

On the first mission (play5200 to 5320): 47,121 batches, 2.17M vertices, every position, normal and colour identical.

Reading it found a VU-model bug: a DIV / SQRT / RSQRT issued while another division was still pending replaced the
pending result, while the hardware's divider makes the new one wait for it (Q written first). The point light's
`div Q, 1/radius` is followed six pairs later by an `rsqrt` and then a `subq` that reads 1/radius: the model gave it
a stale Q, so every point-lit surface was lit with the wrong falloff (brighter). Fixed in the recompiler, the
interpreter and `divideV` (`shino_vu.h`).

## Stage 2b: the main renderer's objects whole, no VU code (2026-11-27)

`SHINO_NATIVE_OBJECTS=1` (with `SHINO_NATIVE_3D=1`): at an object's `MSCAL 0` the flags (1023.x) give the variant the
setup would choose (`variantOf`, the setup's branches at `0x28`-`0x188`) and the light list its types; when the variant
is one computed here (1023 or 554) and every light is read (types 0, 2, 128), none of the object's programs run on the
VU: each batch's setup is made here in place, then the batch drawn as in stage 1, then the first pass's results written
back. On the frame-5200 play, 211,850 of 213,848 objects (99%; the rest variant 1438, or a light type not read).

What that needed, found with two tools:
- **`tools/shino_vu1_live.py`**: for a program's entry, the registers whose value from before it can reach a store, a
  kick or a branch (a lane computed and not used does not count). The setup at pair 0 of every renderer takes nothing
  that matters from before (the variants' reads of vf7.w, vf11.w and the like are lanes stored in a GS register's
  unused word).
- **`SHINO_VU_POISON`** (`shino_vu1.cpp`): each VU1 program run twice from the same state, the second time with the
  registers scrambled (1), the VU1 memory other microcode wrote and the VIF did not since (2), or both (3), the GS
  packets compared field by field (what the GS reads) and the memory after. For every renderer's `MSCAL 0`, the GS
  output is the same: an object does not depend on the registers or memory the programs before it left. The one reader
  of another renderer's leftovers is set `0x4f3f60` (about once a frame, the stale vertices just past a main batch).
  So a native object must leave VU1 memory as the VU would; its registers do not matter.

What the VU leaves, and the native object now leaves too:
- the setup's loops go four vertices at a time, to the batch rounded up to four (`vi11 = vi1 + (4 count + 12 &
  0x7ff0)`), over the stale vertices past the batch: their colours and normals converted, the constant lights added,
  clamped and multiplied by the material; the lit morph's loop and the point lights stop at the batch's end (`vi10`);
- the first pass's clip positions and ADC (to the batch rounded up to two), as stage 1's skip mode wrote them;
- the variant's entry in 1023.y (`isw.y vi12`);
- every result clamped as the VU's arithmetic leaves it (no NaN, nothing past the largest finite): stale vertices can
  hold any bits;
- **TPC**: the next batches are `MSCNT`s, which start where the last program ended. A native object leaves TPC at
  `0x3968` as its variant would; without it the next batch started wherever the last VU program had stopped.

`SHINO_NATIVE_OBJECTS=verify` makes each such program here, then by the VU, and compares the VU1 memory after (the
input buffers 0-551 and the constants 1008-1023; 552-1007 are the output packets) and TPC. The frame-5200 play to 6200
(392,607 programs) and the final fight (406,450): no difference. The checksums of what is drawn (`SHINO_CHECKSUM`: the
GS translator's draws and, separately, the native batches') are the same as with stage 1 (the town at flips 250 to
1000, the final fight at 250 and 500), and the game's RAM the same at frame 6200. 1,000 frames of the town headless: about 8.2 s against 9.6 s with stage 1. A whole mission day (the q93
state's busy day): 7.81M objects, 99.9% whole natively, no fault.

## Stage 2c: the characters whole, and the spotlight (2026-11-27)

**The characters' setup** (image 67, set `0x579a00`) is the main renderer's with skinning in front. The batch comes in
at five quadwords a vertex: position, UV, colour, normal, and a fifth whose four lanes are the bone weights (floats)
with the bones' indices in their low 16 bits (`ilw` reads the index, `lq` the weight). For each vertex the four bones'
rows at 696-699 past the index are blended by the weights (699 the translation), the position (morphed first when
1020.w is not 0, the targets after the five-quadword batch, the normal targets after those) and the normal (over 128)
transformed by the blend, and the batch rewritten in place at four quadwords a vertex (the writes stay behind the
reads). Unlit, the colour is times the material and slot 3 left alone; lit, the main renderer's light code follows,
word for word (`0x570` on: the dispatch, types 2, 1, 128, 129, 130 and the end). Details that matter for exact results:
the first vertex's position and UV are loaded as they are, the others copied by `addx ..., vf0x` (clamped, -0 made
+0); the extra vertices past the batch (to four) are not skinned but are lit and clamped like the main renderer's.

**The spotlight** (type 129, three quadwords: colour, position and radius, direction and cutoff): the point light's
falloff (colour x max(n.d, 0) x (1/|d| - 1/radius), not below 0), added only inside the cone: the cosine between the way
to the light and the spot's direction (rec2's xyz turned into the object's space, times 979.w) against the cutoff
(rec2.w times 979.w), by the sign flag of cutoff - cosine (`fmand` of the MAC flags 4 pairs after the `subx.w`). The
characters of the final fight are lit by it.

Checked at the stop point (`SHINO_NATIVE_3D=verify`): on the frame-5200 play 29,869 character batches (1.04M vertices,
the extras past each batch included) and on the final fight 242,303 (8.95M), and the main renderer's 409,973 there,
every position, UV, normal and colour identical.

**Whole character objects** (`SHINO_NATIVE_OBJECTS=1`): variant 644 (flags `0x9`) natively, the next batches by MSCNT
at `0x3c38`, the output buffers 430-695 (`vi13`, `vi14`) left out of the compare. Verify: the frame-5200 play (92,170
programs) and the final fight (234,448), memory and TPC identical; 74,818 of 76,400 objects of the fight whole
natively (the rest the clipped variant 1113).

One reader sees the difference: set `0x601710`'s sprite program (its output buffers at 416 and 677 overlap the
characters') takes the w lane of a quadword it did not write, which was the characters' last GS packet and is now older
data; it lands in the Q of one corner of a few sprites whose S and T are 0. The primitives of flips 2, 100, 300 and 450
of the fight differ in that Q alone and in nothing drawn, the native batches' checksum is the same as stage 1's (the
town and the fight), and the game's RAM is the same at frame 2306600.

Time, headless: 1,000 frames of the town 8.2 s (9.5 s with stage 1), 500 of the final fight 6.4 s (7.2 s). A whole
mission day: 2.16M character objects, 99.2% whole natively (and the main renderer's 7.81M, 99.9%), no fault.

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
