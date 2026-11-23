"""Shinobido: Way of the Ninja's logo (python docs/branding/make_logo.py): The Bozos' clown as a ninja, drawn with
BozoEngine's logo tool (BozoEngine/tools/branding/make_logo.py: the clown, the PS2 BIOS blue, the boot towers, the
wordmark) plus this game's variant: a night-blue hood (zukin) framing the face, a red headband (hachimaki) with a steel
plate across the brow, its tails flying out behind, a full moon and a shuriken. Generic shapes only, no game art.

Writes docs/branding/shino_icon_1024.png, shino_icon_256.png, shino_banner.png and the window's icon
runtime/app/shinobido.ico (the usual Windows sizes). The font: Exo 2 (SIL OFL 1.1), BOZO_LOGO_FONT=<Exo2[wght].ttf>.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'BozoEngine', 'tools', 'branding'))
import make_logo as logo  # noqa: E402  (BozoEngine's tool)
from PIL import Image, ImageDraw, ImageFilter  # noqa: E402

SS = logo.SS
# Shinobido's colours: a ninja's night-blue hood with a cold rim, a red headband, a steel brow plate, a pale moon.
HOOD = (22, 24, 44)
HOOD_RIM = (78, 92, 150)
BAND = (222, 50, 66)
BAND_DARK = (150, 28, 44)
STEEL = (196, 206, 230)
STEEL_DARK = (92, 104, 140)
MOON = (255, 238, 196)
TAILS = {'side': 1}  # the headband's tails fly right (the icon) or left (the banner, away from the title)


def moon(canvas, x, y, rr):
    """A full moon with a soft glow and two faint craters."""
    def disc(d):
        d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=MOON + (255,))
    logo.glow_shape(canvas, disc, rr * 0.45, 1.2)
    d = ImageDraw.Draw(canvas)
    for dx, dy, k in ((-0.3, -0.2, 0.22), (0.28, 0.25, 0.16), (0.05, 0.42, 0.1)):
        c = (x + dx * rr, y + dy * rr)
        d.ellipse([c[0] - k * rr, c[1] - k * rr, c[0] + k * rr, c[1] + k * rr], fill=(236, 214, 170, 255))


def shuriken(canvas, x, y, rr, angle=15):
    """A four-pointed throwing star: steel blades, a dark centre hole, a cold glow."""
    pts = []
    for k in range(8):
        a = math.radians(angle + k * 45)
        rad = rr if k % 2 == 0 else rr * 0.3
        pts.append((x + rad * math.cos(a), y + rad * math.sin(a)))

    def star(d):
        d.polygon(pts, fill=STEEL + (255,))
    logo.glow_shape(canvas, star, rr * 0.25, 0.9)
    d = ImageDraw.Draw(canvas)
    for k in range(0, 8, 2):  # a darker half on each blade: the bevel
        d.polygon([(x, y), pts[k], pts[(k + 1) % 8]], fill=STEEL_DARK + (255,))
    h = rr * 0.13
    d.ellipse([x - h, y - h, x + h, y + h], fill=HOOD + (255,))


def ninja_back(canvas, cx, cy, r):
    """Behind the head: the headband's two tails flying out to the right, then the hood (a rounded cowl wider than the
    face, falling to the shoulders, a night-blue rim). Drawn where BozoEngine's clown draws fire hair."""
    tails = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(tails)
    s = TAILS['side']
    knot = (cx + s * r * 0.95, cy - r * 0.58)
    for k, (lift, length, width) in enumerate(((0.55, 1.55, 0.2), (0.05, 1.3, 0.17))):
        steps = 24
        upper, lower = [], []
        for i in range(steps + 1):
            t = i / steps
            x = knot[0] + s * t * length * r
            y = knot[1] - lift * r * t + math.sin(t * math.pi * 1.6 + k) * r * 0.14 * t
            w = width * r * (1 - 0.55 * t)
            upper.append((x, y - w / 2))
            lower.append((x, y + w / 2))
        d.polygon(upper + lower[::-1], fill=BAND + (255,))
        d.line(lower, fill=BAND_DARK + (255,), width=max(2, int(r * 0.03)))
    canvas.alpha_composite(tails.filter(ImageFilter.GaussianBlur(r * 0.08)))
    canvas.alpha_composite(tails)

    layer = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for grow, colour in ((0.07, HOOD_RIM), (0.0, HOOD)):
        # The cowl round the head, then the cloth falling from the chin and flaring to the shoulders.
        d.ellipse([cx - r * (1.22 + grow), cy - r * (1.3 + grow), cx + r * (1.22 + grow), cy + r * (1.16 + grow)],
                  fill=colour + (255,))
        d.polygon([(cx - r * (1.0 + grow), cy + r * 0.5), (cx + r * (1.0 + grow), cy + r * 0.5),
                   (cx + r * (1.75 + grow), cy + r * (1.9 + grow)), (cx - r * (1.75 + grow), cy + r * (1.9 + grow))],
                  fill=colour + (255,))
    canvas.alpha_composite(layer.filter(ImageFilter.GaussianBlur(r * 0.1)))
    canvas.alpha_composite(layer)


def ninja_front(canvas, cx, cy, r):
    """Over the face: the hood's brim across the top of the head, then the headband across the brow with its steel plate
    (a rounded rectangle with two rivets) and the knot at the side."""
    d = ImageDraw.Draw(canvas)
    box = [cx - r * 1.05, cy - r * 1.05, cx + r * 1.05, cy + r * 1.05]
    d.chord(box, 205, 335, fill=HOOD + (255,))
    d.arc(box, 205, 335, fill=HOOD_RIM + (255,), width=max(2, int(r * 0.045)))
    top, bottom = cy - r * 0.7, cy - r * 0.47
    half = math.sqrt(max(0.0, 1 - ((top + bottom) / 2 - cy) ** 2 / r ** 2)) * r * 1.02
    d.rounded_rectangle([cx - half, top, cx + half, bottom], radius=r * 0.05, fill=BAND + (255,))
    d.line([(cx - half, bottom), (cx + half, bottom)], fill=BAND_DARK + (255,), width=max(2, int(r * 0.03)))
    pw, ph = r * 0.36, r * 0.17
    py = (top + bottom) / 2

    def plate(dd):
        dd.rounded_rectangle([cx - pw, py - ph, cx + pw, py + ph], radius=r * 0.05, fill=STEEL + (255,))
    logo.glow_shape(canvas, plate, r * 0.06, 0.8)
    d = ImageDraw.Draw(canvas)
    d.rounded_rectangle([cx - pw, py - ph, cx + pw, py + ph], radius=r * 0.05, outline=STEEL_DARK + (255,),
                        width=max(2, int(r * 0.03)))
    for side in (-1, 1):
        rv = r * 0.035
        d.ellipse([cx + side * pw * 0.78 - rv, py - rv, cx + side * pw * 0.78 + rv, py + rv], fill=STEEL_DARK + (255,))
    # The plate's mark: a small four-pointed star (the shuriken again), engraved.
    shuriken_mark = [(cx + math.cos(math.radians(a)) * (ph * 0.7 if k % 2 == 0 else ph * 0.22),
                      py + math.sin(math.radians(a)) * (ph * 0.7 if k % 2 == 0 else ph * 0.22))
                     for k, a in enumerate(range(-90, 270, 45))]
    d.polygon(shuriken_mark, fill=STEEL_DARK + (255,))
    kx, ky = cx + TAILS['side'] * r * 0.95, cy - r * 0.58
    kr = r * 0.13
    d.ellipse([kx - kr, ky - kr, kx + kr, ky + kr], fill=BAND + (255,), outline=BAND_DARK + (255,),
              width=max(2, int(r * 0.03)))


_engine_clown = logo.clown


def ninja_clown(canvas, cx, cy, r, fire=False, **_):
    """BozoEngine's clown with fire=True (no hat, no hair puffs; its fire_hair hook draws ninja_back), then the front."""
    _engine_clown(canvas, cx, cy, r, fire=True)
    ninja_front(canvas, cx, cy, r)


logo.fire_hair = ninja_back
logo.clown = ninja_clown


def icon(px):
    W = px * SS
    bg = logo.background(W, W).convert('RGBA')
    bg.alpha_composite(logo.towers(W * 0.4, W, W, W * 1.02, 9))
    TAILS['side'] = 1
    moon(bg, W * 0.78, W * 0.2, W * 0.12)
    shuriken(bg, W * 0.17, W * 0.18, W * 0.09)
    ninja_clown(bg, W * 0.46, W * 0.55, W * 0.25)
    bg = logo.scanlines(bg, SS * 3)
    mask = Image.new('L', (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, W - 1], radius=W * 0.18, fill=255)
    bg.putalpha(mask)
    return bg.resize((px, px), Image.LANCZOS)


def banner():
    w, h = 2000, 600
    TAILS['side'] = -1
    img = logo.wordmark(w, h, False, title='SHINOBIDO: WAY OF THE NINJA', subtitle='NATIVE PC PORT  ·  THE BOZOS',
                        fire=True, title_size=0.24)
    # The moon over the right end of the title band, drawn at full size on the finished banner (behind nothing).
    big = img.resize((w * SS // 2, h * SS // 2), Image.LANCZOS).convert('RGBA')
    over = Image.new('RGBA', big.size, (0, 0, 0, 0))
    moon(over, big.size[0] * 0.95, big.size[1] * 0.16, big.size[1] * 0.1)
    over.putalpha(over.getchannel('A').point(lambda v: int(v * 0.85)))
    big.alpha_composite(over)
    return big.resize((w, h), Image.LANCZOS)


if __name__ == '__main__':
    big = icon(1024)
    big.save(os.path.join(HERE, 'shino_icon_1024.png'))
    icon(256).save(os.path.join(HERE, 'shino_icon_256.png'))
    app = os.path.join(ROOT, 'runtime', 'app')
    os.makedirs(app, exist_ok=True)
    big.save(os.path.join(app, 'shinobido.ico'), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128),
                                                        (256, 256)])
    banner().save(os.path.join(HERE, 'shino_banner.png'))
    print('shinobido done ->', HERE)
