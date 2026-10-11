#!/usr/bin/env python3
"""Renders Highlight's logo files from one definition of the mark.

    python3 scripts/make-brand.py        (needs Pillow and fontTools)

Writes:
    assets/icons/highlight.ico, highlight.png    app icon (exe, installer, README)
    assets/brand/*.svg, *.png                    mark, icon and logo (wordmark) files
    installer/wizard-*.bmp                       setup wizard pictures

The mark's numbers mirror src/common/logo.cpp, which draws the same shapes in the
app (tray icon, banner icon, window logo). Change both together.
"""

import io
import math
import struct
from pathlib import Path

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
FONT = ROOT / "assets" / "fonts" / "Figtree-SemiBold.ttf"
TAGLINE_FONT = ROOT / "assets" / "fonts" / "Archivo-Medium.ttf"

ORANGE = (0xFF, 0x5A, 0x1F)
INK = (0x0B, 0x0C, 0x0E)
TEXT = (0xEC, 0xED, 0xEF)
MUTED = (0xA0, 0xA6, 0xB0)
PANEL = (0x11, 0x13, 0x17)

# ---------------------------------------------------------------------------
# the mark: 96 x 110 units, clockwise outlines, (x, y, corner radius)

MARK_W, MARK_H = 96.0, 110.0
R_BAR, R_TRI, R_NOTCH, R_NOTCH_EDGE = 2.2, 2.2, 1.75, 0.8
MARK = [
    # left bar: both bars are slanted 18 units over their 29 unit width
    [(0, 18, R_BAR), (29, 0, R_BAR), (29, 92, R_BAR), (0, 110, R_BAR)],
    # right bar, with the notch the triangle points into: edges parallel to the
    # triangle's (an even gap), ending in a short flat tip
    [(67, 18, R_BAR), (96, 0, R_BAR), (96, 92, R_BAR), (67, 110, R_BAR),
     (67, 70.6, R_NOTCH_EDGE), (84.6, 60, R_NOTCH), (84.6, 50, R_NOTCH), (67, 39.4, R_NOTCH_EDGE)],
    # play triangle, centred on the bars
    [(38, 32.75, R_TRI), (73.5, 55, R_TRI), (38, 77.25, R_TRI)],
]


def corner_arcs(poly):
    """Per corner: tangent point in, tangent point out, centre, radius, sweep (1 = clockwise)."""
    out = []
    n = len(poly)
    for i in range(n):
        (ax, ay, _), (px, py, r), (bx, by, _) = poly[i - 1], poly[i], poly[(i + 1) % n]
        u1 = (ax - px, ay - py)
        u2 = (bx - px, by - py)
        l1, l2 = math.hypot(*u1), math.hypot(*u2)
        u1 = (u1[0] / l1, u1[1] / l1)
        u2 = (u2[0] / l2, u2[1] / l2)
        ang = math.acos(max(-1.0, min(1.0, u1[0] * u2[0] + u1[1] * u2[1])))
        d = r / math.tan(ang / 2)
        bis = (u1[0] + u2[0], u1[1] + u2[1])
        bl = math.hypot(*bis)
        cd = r / math.sin(ang / 2)
        c = (px + bis[0] / bl * cd, py + bis[1] / bl * cd)
        t1 = (px + u1[0] * d, py + u1[1] * d)
        t2 = (px + u2[0] * d, py + u2[1] * d)
        # turn direction of the outline at this corner (y down: positive = clockwise)
        cross = (px - ax) * (by - py) - (py - ay) * (bx - px)
        out.append((t1, t2, c, r, 1 if cross > 0 else 0))
    return out


def mark_polygons(seg=16):
    """The mark as point lists (for raster drawing)."""
    polys = []
    for poly in MARK:
        pts = []
        for t1, t2, c, r, _ in corner_arcs(poly):
            a1 = math.atan2(t1[1] - c[1], t1[0] - c[0])
            a2 = math.atan2(t2[1] - c[1], t2[0] - c[0])
            da = (a2 - a1 + math.pi) % (2 * math.pi) - math.pi
            for k in range(seg + 1):
                a = a1 + da * k / seg
                pts.append((c[0] + r * math.cos(a), c[1] + r * math.sin(a)))
        polys.append(pts)
    return polys


def mark_svg_path(ox, oy, s):
    """The mark as an SVG path (exact arcs), placed at (ox, oy) and scaled by s."""
    f = lambda v: f"{v:.3f}".rstrip("0").rstrip(".")
    parts = []
    for poly in MARK:
        arcs = corner_arcs(poly)
        t1 = arcs[0][0]
        d = [f"M{f(ox + t1[0] * s)} {f(oy + t1[1] * s)}"]
        for i, (t1, t2, c, r, sweep) in enumerate(arcs):
            if i:
                d.append(f"L{f(ox + t1[0] * s)} {f(oy + t1[1] * s)}")
            d.append(f"A{f(r * s)} {f(r * s)} 0 0 {sweep} {f(ox + t2[0] * s)} {f(oy + t2[1] * s)}")
        d.append("Z")
        parts.append("".join(d))
    return "".join(parts)


# ---------------------------------------------------------------------------
# raster helpers

SS = 8  # supersampling


def mark_fraction(tile_px):
    """Mark height relative to its tile (same as logo_mark_fraction() in logo.h)."""
    return 0.68 if tile_px <= 24 else 0.62


def draw_mark(draw, cx, cy, height, fill, polys=None):
    s = height / MARK_H
    ox, oy = cx - MARK_W * s / 2, cy - height / 2
    for pts in polys or mark_polygons():
        draw.polygon([(ox + x * s, oy + y * s) for x, y in pts], fill=fill)


def icon_placement(tile):
    """Same as logo_icon_placement() in logo.cpp: scale, x, y, per-outline x shift.
    Tiles up to 48 px are hinted so the bars' upright edges sit on pixel boundaries."""
    s = tile * mark_fraction(tile) / MARK_H
    dx = [0.0, 0.0, 0.0]
    if tile <= 48:
        bar = round(29 * s)
        if bar >= 2 and abs(bar - 29 * s) <= 0.3:
            s = bar / 29
        x = round((tile - MARK_W * s) / 2)
        y = round((tile - MARK_H * s) / 2)
        dx[1] = round(x + 67 * s) - (x + 67 * s)
        dx[2] = round(x + 38 * s) - (x + 38 * s)
    else:
        x = (tile - MARK_W * s) / 2
        y = (tile - MARK_H * s) / 2
    return s, x, y, dx


def draw_tile(draw, x, y, size, scale=1):
    """Orange rounded square with the mark; size is the final (unscaled) size in pixels,
    scale the supersampling factor of the canvas."""
    big = size * scale
    draw.rounded_rectangle((x, y, x + big - 1, y + big - 1), radius=big * 0.25, fill=ORANGE + (255,))
    s, mx, my, dx = icon_placement(size)
    for i, pts in enumerate(mark_polygons()):
        draw.polygon([(x + (mx + dx[i] + px * s) * scale, y + (my + py * s) * scale) for px, py in pts],
                     fill=INK + (255,))


def icon_image(size):
    """The app icon, as on the taskbar."""
    im = Image.new("RGBA", (size * SS, size * SS), (0, 0, 0, 0))
    draw_tile(ImageDraw.Draw(im), 0, 0, size, SS)
    return im.resize((size, size), Image.BOX)


def write_ico(path, sizes):
    """ICO with one PNG image per size (each rendered at its size, not scaled)."""
    blobs = []
    for s in sizes:
        buf = io.BytesIO()
        icon_image(s).save(buf, "PNG", optimize=True)
        blobs.append((s, buf.getvalue()))
    out = io.BytesIO()
    out.write(struct.pack("<HHH", 0, 1, len(blobs)))
    offset = 6 + 16 * len(blobs)
    for s, data in blobs:
        dim = 0 if s >= 256 else s
        out.write(struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset))
        offset += len(data)
    for _, data in blobs:
        out.write(data)
    path.write_bytes(out.getvalue())


# ---------------------------------------------------------------------------
# wordmark: "highlight" in Figtree SemiBold, tracked -0.02 em, x-height centred
# on the mark (as in the artwork)

WORD = "highlight"
TRACK = -0.02


class Wordmark:
    def __init__(self):
        self.font = TTFont(str(FONT))
        self.upm = self.font["head"].unitsPerEm
        self.gs = self.font.getGlyphSet()
        self.cmap = self.font.getBestCmap()
        self.hmtx = self.font["hmtx"]
        self.x_height = self.font["OS/2"].sxHeight / self.upm

    def glyphs(self):
        return [self.cmap[ord(c)] for c in WORD]

    def left_bearing(self):
        """Space before the ink of the first letter, in em."""
        return self.hmtx[self.glyphs()[0]][1] / self.upm

    def width(self, em):
        names = self.glyphs()
        adv = sum(self.hmtx[n][0] for n in names) / self.upm
        return em * (adv + TRACK * (len(names) - 1))

    def svg_path(self, x, baseline, em):
        s = em / self.upm
        parts = []
        for name in self.glyphs():
            pen = SVGPathPen(self.gs, ntos=lambda v: f"{v:.2f}".rstrip("0").rstrip("."))
            self.gs[name].draw(TransformPen(pen, (s, 0, 0, -s, x, baseline)))
            parts.append(pen.getCommands())
            x += self.hmtx[name][0] * s + TRACK * em
        return "".join(parts)

    def draw(self, image, x, baseline, em, fill):
        """Raster version (PIL), same layout as svg_path()."""
        f = ImageFont.truetype(str(FONT), max(1, round(em)))
        d = ImageDraw.Draw(image)
        for ch in WORD:
            d.text((x, baseline), ch, font=f, fill=fill, anchor="ls")
            x += f.getlength(ch) + TRACK * em


def lockup_layout(wm, tile):
    """Tile (orange icon) followed by the wordmark: positions relative to the tile's top-left."""
    em = tile * 0.95
    gap = tile * 0.45
    baseline = tile / 2 + wm.x_height * em / 2
    return em, tile + gap, baseline, tile + gap + wm.width(em)


def svg_doc(w, h, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w:g} {h:g}" '
            f'width="{w:g}" height="{h:g}">{body}</svg>\n')


def hexc(c):
    return "#%02X%02X%02X" % c


def tile_svg(x, y, size):
    s = size * mark_fraction(size) / MARK_H
    return (f'<rect x="{x:g}" y="{y:g}" width="{size:g}" height="{size:g}" rx="{size * 0.25:g}" '
            f'fill="{hexc(ORANGE)}"/>'
            f'<path fill="{hexc(INK)}" d="{mark_svg_path(x + (size - MARK_W * s) / 2, y + size * (1 - mark_fraction(size)) / 2, s)}"/>')


def write_svgs(wm, out):
    # bare mark (white and ink), and the app icon
    (out / "highlight-mark.svg").write_text(svg_doc(MARK_W, MARK_H, f'<path fill="#FFFFFF" d="{mark_svg_path(0, 0, 1)}"/>'))
    (out / "highlight-mark-ink.svg").write_text(svg_doc(MARK_W, MARK_H, f'<path fill="{hexc(INK)}" d="{mark_svg_path(0, 0, 1)}"/>'))
    (out / "highlight-icon.svg").write_text(svg_doc(256, 256, tile_svg(0, 0, 256)))

    # logo: icon + wordmark, for dark and light backgrounds
    tile = 100
    em, wx, baseline, w = lockup_layout(wm, tile)
    for name, color in (("highlight-logo-dark.svg", TEXT), ("highlight-logo-light.svg", INK)):
        body = tile_svg(0, 0, tile) + f'<path fill="{hexc(color)}" d="{wm.svg_path(wx, baseline, em)}"/>'
        (out / name).write_text(svg_doc(math.ceil(w), tile, body))

    # the original artwork: white mark and wordmark (no tile). The gap is measured to
    # the ink of the "h", so the pen starts its side bearing earlier.
    mh = 108.0
    s = mh / MARK_H
    em2 = mh * 0.61
    wx2 = MARK_W * s + mh * 0.356 - wm.left_bearing() * em2
    base2 = mh / 2 + wm.x_height * em2 / 2
    body = (f'<path fill="#FFFFFF" d="{mark_svg_path(0, 0, s)}"/>'
            f'<path fill="#FFFFFF" d="{wm.svg_path(wx2, base2, em2)}"/>')
    (out / "highlight-logo-mono.svg").write_text(svg_doc(math.ceil(wx2 + wm.width(em2)), mh, body))


def lockup_image(wm, tile, color, bg=None, pad=0):
    em, wx, baseline, w = lockup_layout(wm, tile)
    W, H = math.ceil(w + 2 * pad), math.ceil(tile + 2 * pad)
    im = Image.new("RGBA", (W * SS, H * SS), (bg + (255,)) if bg else (0, 0, 0, 0))
    draw_tile(ImageDraw.Draw(im), pad * SS, pad * SS, tile, SS)
    wm.draw(im, (pad + wx) * SS, (pad + baseline) * SS, em * SS, color + (255,))
    return im.resize((W, H), Image.BOX)


def write_pngs(wm, out):
    icon_image(256).save(ROOT / "assets" / "icons" / "highlight.png", optimize=True)
    icon_image(512).save(out / "highlight-icon-512.png", optimize=True)
    lockup_image(wm, 120, TEXT).save(out / "highlight-logo-dark.png", optimize=True)
    lockup_image(wm, 120, INK).save(out / "highlight-logo-light.png", optimize=True)

    # social preview for the repository page (1280 x 640)
    W, H = 1280, 640
    im = Image.new("RGBA", (W, H), INK + (255,))
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    for i in range(60, 0, -1):
        a = int(26 * (1 - i / 60) ** 2)
        gd.ellipse((W / 2 - 9 * i, H / 2 - 40 - 5 * i, W / 2 + 9 * i, H / 2 - 40 + 5 * i), fill=ORANGE + (a,))
    im.alpha_composite(glow)
    lk = lockup_image(wm, 150, TEXT)
    im.alpha_composite(lk, ((W - lk.width) // 2, (H - lk.height) // 2 - 40))
    d = ImageDraw.Draw(im)
    f = ImageFont.truetype(str(TAGLINE_FONT), 34)
    tag = "Hit the key. Keep the moment."
    d.text((W / 2, H / 2 + 90), tag, font=f, fill=MUTED + (255,), anchor="mm")
    im.convert("RGB").save(out / "social-preview.png", optimize=True)


# Inno Setup 6.6+ picture areas at 100/125/150/200 % DPI. The tall picture keeps
# the 164:314 shape; the small one is square. Setup picks the closest size and
# stretches it to the area, so each file must have exactly this shape.
WIZARD_SIZES = {"": (202, 386, 58), "-125": (269, 515, 77), "-150": (336, 643, 97), "-200": (430, 824, 124)}


def write_wizard_images(wm):
    """Inno Setup pictures: the tall one on the welcome/finish pages, the small one in
    the header of the other pages."""
    inst = ROOT / "installer"
    for old in inst.glob("wizard-*.bmp"):
        old.unlink()
    for suffix, (W, H, small_px) in WIZARD_SIZES.items():
        scale = W / 164  # the layout below is designed at 164 x 314
        big = Image.new("RGBA", (W * SS, H * SS), PANEL + (255,))
        tile = round(76 * scale)
        tile += (W - tile) % 2  # centred on whole pixels
        draw_tile(ImageDraw.Draw(big), (W - tile) * SS // 2, round(H * SS * 0.30), tile, SS)
        em = 30 * scale
        x = (W - wm.width(em)) / 2
        baseline = H * 0.30 + tile + 44 * scale
        wm.draw(big, x * SS, baseline * SS, em * SS, TEXT + (255,))
        img = big.resize((W, H), Image.BOX)
        d = ImageDraw.Draw(img)
        f = ImageFont.truetype(str(TAGLINE_FONT), round(10.5 * scale))
        d.text((W / 2, baseline + 26 * scale), "Hit the key.", font=f, fill=MUTED, anchor="mm")
        d.text((W / 2, baseline + 41 * scale), "Keep the moment.", font=f, fill=MUTED, anchor="mm")
        img.convert("RGB").save(inst / f"wizard-image{suffix}.bmp")

        small = Image.new("RGBA", (small_px, small_px), (255, 255, 255, 255))
        side = round(small_px * 48 / 58)
        side += (small_px - side) % 2  # centred on whole pixels
        icon = icon_image(side)
        small.alpha_composite(icon, ((small_px - icon.width) // 2, (small_px - icon.height) // 2))
        small.convert("RGB").save(inst / f"wizard-small{suffix}.bmp")


def main():
    out = ROOT / "assets" / "brand"
    out.mkdir(exist_ok=True)
    wm = Wordmark()
    write_ico(ROOT / "assets" / "icons" / "highlight.ico", [16, 20, 24, 32, 40, 48, 64, 96, 128, 256])
    write_svgs(wm, out)
    write_pngs(wm, out)
    write_wizard_images(wm)
    print("brand files written")


if __name__ == "__main__":
    main()
