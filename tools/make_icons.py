#!/usr/bin/env python3
"""Generate the CAD ribbon/browser icons as 16-colour (4bpp) Windows 3.x BMPs.

Deterministic: running it twice produces byte-identical output.
Outputs (relative to the repo root):
  res/icons/*.BMP       4bpp BITMAPINFOHEADER DIBs, standard VGA palette
  res/icons.rc          IDB_<NAME> BITMAP "res/icons/<NAME>.BMP" lines
  res/icon_ids.h        #define IDB_<NAME> 6xx
  res/icons-preview.png contact sheet at 3x zoom (for human review only)
Colour index 13 (magenta) is transparent.
"""
import os
import struct
import sys

from PIL import Image, ImageDraw

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

# Standard VGA palette in Windows order.
PALETTE = [
    (0x00, 0x00, 0x00), (0x80, 0x00, 0x00), (0x00, 0x80, 0x00), (0x80, 0x80, 0x00),
    (0x00, 0x00, 0x80), (0x80, 0x00, 0x80), (0x00, 0x80, 0x80), (0xC0, 0xC0, 0xC0),
    (0x80, 0x80, 0x80), (0xFF, 0x00, 0x00), (0x00, 0xFF, 0x00), (0xFF, 0xFF, 0x00),
    (0x00, 0x00, 0xFF), (0xFF, 0x00, 0xFF), (0x00, 0xFF, 0xFF), (0xFF, 0xFF, 0xFF),
]
K, MR, DG, OL, NV, PU, TL, LG, DK, R, G, Y, B, T, C, W = range(16)

# Pixel-art character map.
CH = {".": T, "k": K, "m": MR, "g": DG, "o": OL, "n": NV, "p": PU, "t": TL,
      "l": LG, "d": DK, "r": R, "G": G, "y": Y, "b": B, "c": C, "w": W}

ID_BASE = 600


# ----------------------------------------------------------------- helpers

def new(n):
    img = Image.new("P", (n, n), T)
    img.putpalette([v for rgb in PALETTE for v in rgb])
    return img, ImageDraw.Draw(img)


def art(rows):
    n = len(rows)
    img, d = new(n)
    for y, row in enumerate(rows):
        assert len(row) == n, (n, y, row)
        for x, ch in enumerate(row):
            img.putpixel((x, y), CH[ch])
    return img, d


def overlay(img, x0, y0, rows):
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            if ch != ".":
                img.putpixel((x0 + x, y0 + y), CH[ch])


def block(d, x, y, w, h, k, top=Y, front=Y, side=OL, edge=K):
    """Oblique (45 degree) box. (x, y) = top-left of the back-top edge
    projection; w/h = front face size; k = depth offset in pixels."""
    fy = y + k
    d.polygon([(x + w, fy), (x + w + k, y), (x + w + k, y + h), (x + w, fy + h)],
              fill=side, outline=edge)
    d.polygon([(x, fy), (x + k, y), (x + w + k, y), (x + w, fy)], fill=top, outline=edge)
    d.rectangle([x, fy, x + w, fy + h], fill=front, outline=edge)


def wire_block(d, x, y, w, h, k, c=K, hidden=None):
    fy = y + k
    if hidden is not None:
        for i in range(0, h + 1, 2):
            d.point((x + k, y + i), fill=hidden)
        for i in range(0, w + 1, 2):
            d.point((x + k + i, y + h), fill=hidden)
        for i in range(0, k + 1, 2):
            d.point((x + i, fy + h - i), fill=hidden)
    d.rectangle([x, fy, x + w, fy + h], outline=c)
    d.line([(x, fy), (x + k, y), (x + w + k, y), (x + w, fy)], fill=c)
    d.line([(x + w + k, y), (x + w + k, y + h), (x + w, fy + h)], fill=c)


def cylinder(img, d, x, y, w, h, eh, top=W, body=Y, shade=OL, edge=K):
    """Upright cylinder: bounding box x..x+w, y..y+h; eh = ellipse height."""
    d.ellipse([x, y + h - eh, x + w, y + h], fill=body, outline=edge)
    d.rectangle([x, y + eh // 2, x + w, y + h - eh // 2], fill=body)
    sx = x + (w * 2) // 3
    for py in range(y + eh // 2, y + h + 1):
        for px in range(sx, x + w):
            if img.getpixel((px, py)) == body:
                img.putpixel((px, py), shade)
    d.line([(x, y + eh // 2), (x, y + h - eh // 2)], fill=edge)
    d.line([(x + w, y + eh // 2), (x + w, y + h - eh // 2)], fill=edge)
    d.ellipse([x, y, x + w, y + eh], fill=top, outline=edge)


def arrow_head(d, tip, direction, size, c):
    x, y = tip
    s = size
    if direction == "up":
        pts = [(x, y), (x - s, y + s), (x + s, y + s)]
    elif direction == "down":
        pts = [(x, y), (x - s, y - s), (x + s, y - s)]
    elif direction == "left":
        pts = [(x, y), (x + s, y - s), (x + s, y + s)]
    else:
        pts = [(x, y), (x - s, y - s), (x - s, y + s)]
    d.polygon(pts, fill=c, outline=c)


def dot(d, x, y, c=K, ring=None):
    d.rectangle([x - 1, y - 1, x + 1, y + 1], fill=c, outline=ring if ring is not None else c)


# ----------------------------------------------------------- large (32x32)

def L_SKETCH():
    img, d = new(32)
    # grid plane (parallelogram)
    plane = [(1, 30), (10, 17), (31, 17), (22, 30)]
    d.polygon(plane, fill=W, outline=DK)
    for i in range(1, 4):
        yy = 17 + i * 13 // 4
        xs = round(1 + 9 * (30 - yy) / 13)
        d.line([(xs, yy), (xs + 21, yy)], fill=LG)
    for i in range(1, 5):
        xb = 1 + i * 21 // 5
        d.line([(xb, 29), (xb + 8, 18)], fill=LG)
    d.polygon(plane, outline=DK)
    # sketched blue curve on the plane, ending at the pencil tip
    for o in (0, 1):
        d.line([(5, 27 + o), (9, 23 + o), (13, 25 + o), (16, 22 + o)], fill=B)
    # pencil lying at 45 degrees, tip at (16, 22)
    d.polygon([(16, 22), (17, 16), (22, 21)], fill=W, outline=DK)       # wood cone
    d.polygon([(16, 22), (16, 20), (18, 22)], fill=K, outline=K)        # lead
    d.polygon([(17, 16), (22, 21), (31, 12), (26, 7)], fill=B, outline=NV)
    d.line([(19, 17), (27, 9)], fill=C)
    d.polygon([(26, 7), (31, 12), (31, 5), (29, 4)], fill=LG, outline=DK)  # ferrule
    d.line([(28, 6), (31, 9)], fill=DK)
    return img


def L_FINISH():
    img, d = new(32)
    pts = [(3, 17), (8, 12), (13, 17), (26, 4), (31, 9), (13, 27)]
    d.polygon([(x, y + 1) for x, y in pts], fill=DG)
    d.polygon(pts, fill=G, outline=DG)
    d.line([(4, 17), (8, 13), (13, 18), (26, 5)], fill=W)
    return img


def L_EXTRUDE():
    img, d = new(32)
    # blue sketch profile (flat parallelogram) on the base
    d.polygon([(1, 29), (8, 22), (26, 22), (19, 29)], outline=B)
    d.polygon([(2, 30), (9, 23), (27, 23), (20, 30)], outline=B)
    # extruded block rising from the profile
    block(d, 3, 9, 14, 13, 6)
    # extrusion arrow
    d.line([(28, 22), (28, 6)], fill=NV, width=2)
    arrow_head(d, (28, 1), "up", 3, NV)
    d.polygon([(28, 1), (25, 5), (31, 5)], fill=NV, outline=NV)
    return img


def L_REVOLVE():
    img, d = new(32)
    # revolved body
    cylinder(img, d, 6, 6, 19, 22, 8)
    # dash-dot axis through the centre
    for yy in (0, 1, 3, 5, 6, 7, 28, 29, 31):
        d.point((15, yy), fill=K)
    d.point((15, 10), fill=K)
    # blue sweep arrow wrapping round the front of the body
    d.arc([0, 12, 31, 24], 0, 180, fill=B)
    d.arc([0, 13, 31, 25], 0, 180, fill=B)
    d.arc([1, 13, 30, 24], 0, 180, fill=NV)
    d.polygon([(0, 13), (0, 18), (4, 15)], fill=B, outline=B)
    d.polygon([(28, 18), (31, 13), (31, 18)], fill=B, outline=NV)
    return img


def L_BOX():
    img, d = new(32)
    block(d, 2, 3, 18, 18, 10)
    return img


FX = [
    "..........nnnn........",
    ".........nbbbbn.......",
    "........nbbnnbbn......",
    "........nbbn..n.......",
    ".......nbbn...........",
    "....nnnbbnnnn.........",
    "...nbbbbbbbbbn........",
    "....nnbbbnnnn.........",
    "......nbbn............",
    "......nbbn...nn....nn.",
    ".....nbbn...nbbn..nbbn",
    ".....nbbn....nbbnnbbn.",
    ".....nbbn.....nbbbbn..",
    "....nbbn.......nbbn...",
    "....nbbn......nbbbbn..",
    "....nbbn.....nbbnnbbn.",
    "...nbbn.....nbbn..nbbn",
    "...nbbn......nn....nn.",
    "..nbbn................",
    "nnbbn.................",
    "nbbn..................",
    ".nn...................",
]


def L_PARAMS():
    img, d = new(32)
    # parameter table behind
    d.rectangle([16, 1, 31, 17], fill=W, outline=DK)
    d.rectangle([16, 1, 31, 4], fill=B, outline=DK)
    for yy in (8, 12):
        d.line([(18, yy), (29, yy)], fill=LG)
    d.line([(23, 5), (23, 16)], fill=LG)
    overlay(img, 1, 9, FX)
    return img


def L_PLACE():
    img, d = new(32)
    block(d, 2, 10, 16, 14, 7)
    # green plus badge
    d.rectangle([19, 1, 30, 12], fill=W, outline=DG)
    d.rectangle([23, 3, 26, 10], fill=G, outline=DG)
    d.rectangle([21, 5, 28, 8], fill=G, outline=DG)
    d.rectangle([24, 4, 25, 9], fill=G)
    d.rectangle([22, 6, 27, 7], fill=G)
    return img


def gear(d, cx, cy, r_out, r_in, teeth, fill=LG, edge=K, hole=W):
    import math
    pts = []
    n = teeth * 4
    for i in range(n):
        a = 2 * math.pi * i / n
        rr = r_out if (i % 4) in (0, 1) else r_in
        pts.append((round(cx + rr * math.cos(a)), round(cy + rr * math.sin(a))))
    d.polygon(pts, fill=fill, outline=edge)
    hr = max(1, r_in // 2)
    d.ellipse([cx - hr, cy - hr, cx + hr, cy + hr], fill=hole, outline=edge)


def L_MACRO():
    img, d = new(32)
    # page with folded corner
    d.polygon([(4, 1), (19, 1), (25, 7), (25, 30), (4, 30)], fill=W, outline=DK)
    d.polygon([(19, 1), (19, 7), (25, 7)], fill=LG, outline=DK)
    for i, yy in enumerate(range(5, 22, 3)):
        x1 = 7 + (i % 3) * 2
        d.line([(x1, yy), (min(17, x1 + 6 + (i * 3) % 5), yy)], fill=B)
    gear(d, 21, 23, 8, 6, 8, fill=DK, edge=K, hole=DK)
    d.ellipse([18, 20, 24, 26], fill=LG, outline=K)
    d.rectangle([20, 22, 22, 24], fill=W, outline=K)
    return img


def L_HOME():
    img, d = new(32)
    # roof
    d.polygon([(1, 15), (15, 2), (30, 15), (27, 15), (15, 6), (4, 15)], fill=NV, outline=NV)
    d.polygon([(3, 15), (15, 4), (28, 15)], fill=B, outline=NV)
    # chimney (pokes out of the roof slope)
    d.rectangle([21, 5, 24, 9], fill=DK, outline=K)
    d.polygon([(3, 15), (15, 4), (28, 15)], fill=B, outline=NV)
    d.line([(15, 6), (25, 15)], fill=C)
    # walls
    d.rectangle([5, 15, 26, 29], fill=W, outline=DK)
    # small cube instead of a window (home view of the model)
    block(d, 8, 17, 6, 6, 3)
    # door
    d.rectangle([18, 20, 23, 29], fill=Y, outline=K)
    d.point((22, 25), fill=K)
    return img


# ----------------------------------------------------------- small (16x16)

def S_LINE():
    img, d = new(16)
    d.line([(3, 12), (12, 3)], fill=B)
    d.line([(4, 12), (12, 4)], fill=B)
    for x, y in ((2, 13), (13, 2)):
        d.rectangle([x - 1, y - 1, x + 1, y + 1], fill=W, outline=K)
    return img


def S_RECT():
    img, d = new(16)
    d.rectangle([2, 4, 13, 11], outline=B)
    d.rectangle([3, 5, 12, 10], outline=B)
    for x, y in ((2, 4), (13, 11)):
        d.rectangle([x - 1, y - 1, x + 1, y + 1], fill=W, outline=K)
    return img


def S_CIRCLE():
    img, d = new(16)
    d.ellipse([1, 1, 14, 14], outline=B)
    d.ellipse([2, 2, 13, 13], outline=B)
    d.rectangle([7, 7, 8, 8], fill=K)
    return img


def S_POLY():
    img, d = new(16)
    hexa = [(4, 2), (11, 2), (15, 8), (11, 13), (4, 13), (0, 8)]
    d.polygon(hexa, outline=B)
    d.polygon([(4, 3), (11, 3), (14, 8), (11, 12), (4, 12), (1, 8)], outline=B)
    dot(d, 7, 8, K)
    return img


def S_PROPS():
    img, d = new(16)
    d.rectangle([1, 0, 12, 15], fill=W, outline=DK)
    d.rectangle([2, 1, 11, 2], fill=B)
    for yy in (5, 8, 11):
        d.rectangle([3, yy - 1, 4, yy], fill=NV)
        d.line([(6, yy), (10, yy)], fill=DK)
    # pencil at the lower right
    d.line([(9, 14), (15, 8)], fill=Y)
    d.line([(10, 14), (15, 9)], fill=OL)
    d.point((9, 15), fill=K)
    return img


def _curved_arrow(mirror):
    img = art([
        "................",
        "....b...........",
        "...bb...........",
        "..bbbbbbbbb.....",
        ".bbbbbbbbbbbb...",
        "..bbbbbbbbbbbb..",
        "...bb......nbbb.",
        "....b.......nbb.",
        "............nbb.",
        "............nbb.",
        "...........nbb..",
        ".........nnbbb..",
        "......bbbbbbb...",
        "......bbbbbn....",
        "................",
        "................",
    ])[0]
    if mirror:
        img = img.transpose(Image.FLIP_LEFT_RIGHT)
    return img


def S_UNDO():
    return _curved_arrow(False)


def S_REDO():
    return _curved_arrow(True)


def S_WIRE():
    img, d = new(16)
    wire_block(d, 1, 2, 9, 9, 4, c=K, hidden=DK)
    return img


def S_SHADED():
    img, d = new(16)
    block(d, 1, 2, 9, 9, 4)
    return img


def S_FIT():
    img, d = new(16)
    for (x, y, sx, sy) in ((0, 0, 1, 1), (15, 0, -1, 1), (0, 15, 1, -1), (15, 15, -1, -1)):
        d.line([(x, y), (x + 4 * sx, y)], fill=K)
        d.line([(x, y), (x, y + 4 * sy)], fill=K)
        d.line([(x + sx, y + sy), (x + 3 * sx, y + 3 * sy)], fill=K)
    block(d, 4, 4, 5, 5, 3)
    return img


def S_COINC():
    img, d = new(16)
    d.line([(1, 14), (8, 7)], fill=B)
    d.line([(1, 13), (7, 7)], fill=B)
    d.line([(8, 7), (15, 9)], fill=B)
    d.line([(8, 8), (15, 10)], fill=B)
    d.ellipse([5, 4, 11, 10], fill=Y, outline=K)
    d.point((8, 7), fill=K)
    return img


def S_ALIGN():
    img, d = new(16)
    for yy in range(0, 16, 2):
        d.point((3, yy), fill=R)
    d.rectangle([3, 2, 13, 6], fill=Y, outline=K)
    d.rectangle([3, 9, 9, 13], fill=Y, outline=K)
    d.line([(4, 6), (12, 6)], fill=OL)
    d.line([(4, 13), (8, 13)], fill=OL)
    return img


def S_DIST():
    img, d = new(16)
    d.line([(1, 3), (1, 13)], fill=K)
    d.line([(14, 3), (14, 13)], fill=K)
    d.line([(2, 8), (13, 8)], fill=B)
    arrow_head(d, (2, 8), "left", 3, B)
    arrow_head(d, (13, 8), "right", 3, B)
    return img


def S_FIX():
    return art([
        "................",
        ".....dddddd.....",
        "....dllllllk....",
        "...dlk....dlk...",
        "...dlk....dlk...",
        "...dlk....dlk...",
        "...dlk....dlk...",
        "..kkkkkkkkkkkk..",
        "..kyyyyyyyyyok..",
        "..kyywwyyyyyok..",
        "..kyyyykkyyyok..",
        "..kyyyykkyyyok..",
        "..kyyyyykyyyok..",
        "..kyyyyyyyyyok..",
        "..kooooooooook..",
        "..kkkkkkkkkkkk..",
    ])[0]


def S_DELETE():
    img, d = new(16)
    for o in (-1, 0, 1):
        d.line([(2 + o, 2), (13 + o, 13)], fill=R)
        d.line([(13 + o, 2), (2 + o, 13)], fill=R)
    d.line([(1, 1), (2, 1)], fill=MR)
    return img


def S_MOVE():
    img, d = new(16)
    d.line([(7, 2), (7, 13)], fill=NV, width=1)
    d.line([(8, 2), (8, 13)], fill=NV)
    d.line([(2, 7), (13, 7)], fill=NV)
    d.line([(2, 8), (13, 8)], fill=NV)
    d.polygon([(7, 0), (8, 0), (11, 3), (4, 3)], fill=B, outline=NV)
    d.polygon([(7, 15), (8, 15), (11, 12), (4, 12)], fill=B, outline=NV)
    d.polygon([(0, 7), (0, 8), (3, 11), (3, 4)], fill=B, outline=NV)
    d.polygon([(15, 7), (15, 8), (12, 11), (12, 4)], fill=B, outline=NV)
    return img


def S_GROUND():
    img, d = new(16)
    block(d, 3, 1, 7, 6, 3)
    d.line([(0, 11), (15, 11)], fill=K)
    for x in range(0, 16, 3):
        d.line([(x + 3, 12), (x, 15)], fill=DK)
    return img


def S_RELOAD():
    return art([
        "................",
        ".....gggggg.....",
        "...ggGGGGGGgg.G.",
        "..gGGg....gGGgG.",
        "..gGg.......gGG.",
        ".gGg.......GGGG.",
        ".gGg............",
        ".gGg............",
        "............gGg.",
        "............gGg.",
        ".GGGG.......gGg.",
        ".GGg.......gGg..",
        ".GgGGg....gGGg..",
        ".G.ggGGGGGGgg...",
        ".....gggggg.....",
        "................",
    ])[0]


def S_UNITS():
    img, d = new(16)
    d.polygon([(0, 11), (11, 0), (15, 4), (4, 15)], fill=Y, outline=K)
    for i in range(1, 6):
        x = 1 + i * 2
        y = 11 - i * 2 + 1
        ln = 2 if i % 2 else 3
        d.line([(x, y), (x + ln, y + ln)], fill=K)
    return img


def _view_cube(face):
    img, d = new(16)
    top = Y if face != "top" else B
    front = Y if face != "front" else B
    side = OL if face != "side" else NV
    if face == "iso":
        top, front, side = W, Y, OL
    block(d, 1, 1, 9, 9, 5, top=top, front=front, side=side)
    if face == "iso":
        d.point((6, 6), fill=R)
        d.rectangle([5, 5, 7, 7], outline=R)
        d.point((6, 6), fill=W)
    return img


def S_FRONT():
    return _view_cube("front")


def S_TOP():
    return _view_cube("top")


def S_SIDE():
    return _view_cube("side")


def S_ISO():
    return _view_cube("iso")


def S_DRAWING():
    img, d = new(16)
    d.rectangle([0, 2, 15, 13], fill=W, outline=K)
    d.rectangle([1, 3, 14, 12], outline=LG)
    d.rectangle([9, 10, 14, 12], fill=LG, outline=DK)
    # front view (rect with a hole) and a side view
    d.rectangle([3, 5, 7, 9], outline=NV)
    d.point((5, 7), fill=NV)
    d.rectangle([10, 5, 12, 8], outline=NV)
    return img


def S_GRID():
    img, d = new(16)
    d.rectangle([1, 1, 14, 14], fill=W, outline=DK)
    for v in range(4, 14, 3):
        d.line([(v, 2), (v, 13)], fill=LG)
        d.line([(2, v), (13, v)], fill=LG)
    for v in (4, 7, 10, 13):
        for u in (4, 7, 10, 13):
            d.point((u, v), fill=B)
    return img


def S_ORBIT():
    img, d = new(16)
    d.ellipse([4, 4, 11, 11], fill=Y, outline=K)
    d.point((6, 6), fill=W)
    d.arc([0, 3, 15, 12], 190, 160, fill=B)
    d.arc([0, 4, 15, 13], 190, 160, fill=NV)
    arrow_head(d, (2, 9), "down", 2, B)
    d.ellipse([4, 4, 11, 11], outline=K)
    return img


def S_PAN():
    return art([
        ".......kk.......",
        "...kk.kwwkkk....",
        "..kwwkkwwkwwk...",
        "..kwwkkwwkwwk.k.",
        "...kwwkwwkwwkkwk",
        "...kwwkwwkwwkwwk",
        ".kk.kwwwwwwwkwwk",
        "kwwkkwwwwwwwwwwk",
        "kwwwkwwwwwwwwwk.",
        ".kwwwwwwwwwwwwk.",
        "..kwwwwwwwwwwwk.",
        "..kwwwwwwwwwwk..",
        "...kwwwwwwwwwk..",
        "....kwwwwwwwk...",
        ".....kwwwwwwk...",
        ".....kkkkkkkk...",
    ])[0]


def S_ZOOM():
    img, d = new(16)
    d.line([(9, 9), (14, 14)], fill=K, width=1)
    d.line([(10, 9), (15, 14)], fill=DK)
    d.line([(9, 10), (14, 15)], fill=DK)
    d.line([(10, 10), (14, 14)], fill=K)
    d.ellipse([0, 0, 10, 10], fill=C, outline=K)
    d.ellipse([1, 1, 9, 9], outline=DK)
    d.line([(3, 5), (7, 5)], fill=NV)
    d.line([(5, 3), (5, 7)], fill=NV)
    d.point((3, 3), fill=W)
    return img


# --------------------------------------------------------- browser (16x16)

def B_PART():
    img, d = new(16)
    block(d, 1, 2, 9, 9, 4)
    return img


def _plane(d, pts, fill=Y, edge=OL):
    d.polygon(pts, fill=fill, outline=edge)


def B_ORIGIN():
    img, d = new(16)
    # three mutually perpendicular work planes meeting at the origin (5, 10)
    _plane(d, [(5, 10), (5, 1), (14, 1), (14, 10)], fill=Y)          # XY
    _plane(d, [(5, 10), (5, 1), (1, 5), (1, 14)], fill=W)            # YZ
    _plane(d, [(5, 10), (14, 10), (10, 14), (1, 14)], fill=Y)        # XZ
    d.point((5, 10), fill=R)
    return img


def B_PLANE():
    img, d = new(16)
    _plane(d, [(0, 13), (5, 3), (15, 3), (10, 13)])
    d.line([(1, 12), (5, 4)], fill=W)
    return img


def B_AXIS():
    img, d = new(16)
    d.line([(2, 13), (13, 2)], fill=OL)
    d.line([(2, 14), (14, 2)], fill=Y)
    d.line([(3, 14), (14, 3)], fill=OL)
    dot(d, 2, 13, Y, OL)
    dot(d, 13, 2, Y, OL)
    return img


def B_POINT():
    img, d = new(16)
    d.line([(7, 1), (7, 14)], fill=OL)
    d.line([(1, 7), (14, 7)], fill=OL)
    d.ellipse([4, 4, 10, 10], fill=Y, outline=K)
    d.point((6, 6), fill=W)
    return img


def B_SKETCH():
    img, d = new(16)
    d.polygon([(0, 14), (5, 4), (15, 4), (10, 14)], fill=W, outline=DK)
    for o in (0, 1):
        d.line([(2, 11 + o), (5, 7 + o), (8, 10 + o), (12, 6 + o)], fill=B)
    return img


def B_PROFILE():
    img, d = new(16)
    shape = [(1, 13), (1, 3), (8, 3), (8, 7), (14, 7), (14, 13)]
    d.polygon(shape, fill=C, outline=B)
    for o in range(-12, 16, 3):
        for x in range(2, 14):
            y = x + o
            if 0 <= y < 16 and img.getpixel((x, y)) == C:
                img.putpixel((x, y), W)
    d.polygon(shape, outline=B)
    return img


def B_EXTRUDE():
    img, d = new(16)
    d.polygon([(0, 15), (4, 11), (12, 11), (8, 15)], outline=B)
    block(d, 1, 1, 7, 9, 4)
    return img


def B_REVOLVE():
    img, d = new(16)
    cylinder(img, d, 2, 1, 11, 14, 5)
    return img


def B_EOP():
    img, d = new(16)
    d.rectangle([1, 6, 14, 9], fill=R, outline=MR)
    d.line([(2, 7), (13, 7)], fill=W)
    return img


def B_VERTEX():
    img, d = new(16)
    wire_block(d, 1, 2, 9, 9, 4, c=DK)
    d.rectangle([8, 4, 12, 8], fill=R, outline=K)
    return img


def B_EDGE():
    img, d = new(16)
    wire_block(d, 1, 2, 9, 9, 4, c=DK)
    d.line([(1, 6), (10, 6)], fill=B)
    d.line([(1, 7), (10, 7)], fill=B)
    d.line([(10, 6), (14, 2)], fill=B)
    d.line([(10, 7), (14, 3)], fill=B)
    return img


def B_CONSTR():
    return art([
        "................",
        "................",
        ".....kkkkkk.....",
        "....kddddddk....",
        "....kdk..kdk....",
        "....kdk..kdk....",
        "....kdk..kdk....",
        "..kkkkkkkkkkkk..",
        "..kbbbbbbbbbnk..",
        "..kbcbbkkbbbnk..",
        "..kbbbbkkbbbnk..",
        "..kbbbbbkbbbnk..",
        "..kbbbbbbbbbnk..",
        "..knnnnnnnnnnk..",
        "..kkkkkkkkkkkk..",
        "................",
    ])[0]


def B_COMP():
    img, d = new(16)
    block(d, 0, 6, 6, 6, 3)
    block(d, 6, 0, 6, 6, 3, top=W, front=C, side=TL)
    return img


def B_ERROR():
    return art([
        ".....mmmmmm.....",
        "...mmrrrrrrmm...",
        "..mrrrrrrrrrrm..",
        ".mrrrrrwwrrrrrm.",
        ".mrrrrrwwrrrrrm.",
        "mrrrrrrwwrrrrrrm",
        "mrrrrrrwwrrrrrrm",
        "mrrrrrrwwrrrrrrm",
        "mrrrrrrwwrrrrrrm",
        "mrrrrrrrrrrrrrrm",
        "mrrrrrrwwrrrrrrm",
        ".mrrrrrwwrrrrrm.",
        ".mrrrrrrrrrrrrm.",
        "..mrrrrrrrrrrm..",
        "...mmrrrrrrmm...",
        ".....mmmmmm.....",
    ])[0]


def B_FOLDER():
    return art([
        "................",
        "................",
        ".ooooo..........",
        "oyyyyyo.........",
        "oyyyyyyoooooooo.",
        "oyyyyyyyyyyyyyo.",
        "owwwwwwwwwwwwwwo",
        "oywwwwwwwwwwwwwo",
        "oyyyyyyyyyyyyyyo",
        "oyyyyyyyyyyyyyyo",
        "oyyyyyyyyyyyyyyo",
        "oyyyyyyyyyyyyyyo",
        "oyyyyyyyyyyyyyyo",
        "oooooooooooooooo",
        "................",
        "................",
    ])[0]


LARGE = ["L_SKETCH", "L_FINISH", "L_EXTRUDE", "L_REVOLVE", "L_BOX", "L_PARAMS",
         "L_PLACE", "L_MACRO", "L_HOME"]
SMALL = ["S_LINE", "S_RECT", "S_CIRCLE", "S_POLY", "S_PROPS", "S_UNDO", "S_REDO",
         "S_WIRE", "S_SHADED", "S_FIT", "S_COINC", "S_ALIGN", "S_DIST", "S_FIX",
         "S_DELETE", "S_MOVE", "S_GROUND", "S_RELOAD", "S_UNITS", "S_FRONT", "S_TOP",
         "S_SIDE", "S_ISO", "S_DRAWING", "S_GRID", "S_ORBIT", "S_PAN", "S_ZOOM"]
BROWSER = ["B_PART", "B_ORIGIN", "B_PLANE", "B_AXIS", "B_POINT", "B_SKETCH",
           "B_PROFILE", "B_EXTRUDE", "B_REVOLVE", "B_EOP", "B_VERTEX", "B_EDGE",
           "B_CONSTR", "B_COMP", "B_ERROR", "B_FOLDER"]
ALL = LARGE + SMALL + BROWSER


# --------------------------------------------------------------- writers

def write_bmp4(img, path):
    w, h = img.size
    px = img.load()
    stride = ((w * 4 + 31) // 32) * 4
    rows = []
    for y in range(h - 1, -1, -1):  # bottom-up
        row = bytearray(stride)
        for x in range(w):
            v = px[x, y]
            assert 0 <= v < 16, (path, x, y, v)
            if x & 1:
                row[x >> 1] |= v
            else:
                row[x >> 1] |= v << 4
        rows.append(bytes(row))
    bits = b"".join(rows)
    pal = b"".join(struct.pack("<BBBB", b, g, r, 0) for (r, g, b) in PALETTE)
    off = 14 + 40 + len(pal)
    data = (struct.pack("<2sIHHI", b"BM", off + len(bits), 0, 0, off)
            + struct.pack("<IiiHHIIiiII", 40, w, h, 1, 4, 0, len(bits), 0, 0, 16, 16)
            + pal + bits)
    with open(path, "wb") as f:
        f.write(data)


def write_text(path, text):
    with open(path, "w", newline="\n") as f:
        f.write(text)


def preview(images, path, zoom=3):
    groups = [LARGE, SMALL, BROWSER]
    cell = 34 * zoom + 6
    label_h = 12
    cols = 10
    nrows = sum((len(g) + cols - 1) // cols for g in groups)
    sheet = Image.new("RGB", (cols * cell + 8, nrows * (cell + label_h) + 8), (0xC0, 0xC0, 0xC0))
    sd = ImageDraw.Draw(sheet)
    row = 0
    for g in groups:
        for i, name in enumerate(g):
            r, c = row + i // cols, i % cols
            img = images[name].copy()
            img.putpalette([v for rgb in PALETTE[:13] + [PALETTE[LG]] + PALETTE[14:] for v in rgb])
            img = img.convert("RGB")
            img = img.resize((img.width * zoom, img.height * zoom), Image.NEAREST)
            x = 4 + c * cell + (cell - img.width) // 2
            y = 4 + r * (cell + label_h) + (cell - img.height) // 2
            sheet.paste(img, (x, y))
            sd.text((4 + c * cell + 2, 4 + r * (cell + label_h) + cell), name, fill=(0, 0, 0))
        row += (len(g) + cols - 1) // cols
    sheet.save(path, optimize=False)


def main():
    out_dir = os.path.join(ROOT, "res", "icons")
    os.makedirs(out_dir, exist_ok=True)
    mod = sys.modules[__name__]
    images = {}
    for name in ALL:
        img = getattr(mod, name)()
        want = 32 if name.startswith("L_") else 16
        assert img.size == (want, want), (name, img.size)
        images[name] = img
        write_bmp4(img, os.path.join(out_dir, name + ".BMP"))

    rc = ["/* Generated by tools/make_icons.py - do not edit. */\n"]
    hdr = ["/* Generated by tools/make_icons.py - do not edit. */\n",
           "#ifndef ICON_IDS_H\n#define ICON_IDS_H\n\n"]
    for i, name in enumerate(ALL):
        rc.append('IDB_%s BITMAP "res/icons/%s.BMP"\n' % (name, name))
        hdr.append("#define IDB_%-12s %d\n" % (name, ID_BASE + i))
    hdr.append("\n#define IDB_ICON_FIRST %d\n#define IDB_ICON_LAST  %d\n"
               % (ID_BASE, ID_BASE + len(ALL) - 1))
    hdr.append("\n#endif\n")
    write_text(os.path.join(ROOT, "res", "icons.rc"), "".join(rc))
    write_text(os.path.join(ROOT, "res", "icon_ids.h"), "".join(hdr))
    preview(images, os.path.join(ROOT, "res", "icons-preview.png"))
    print("wrote %d icons" % len(ALL))


if __name__ == "__main__":
    main()
