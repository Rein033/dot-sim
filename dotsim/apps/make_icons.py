"""Pixel-art icons for the preloaded apps: 32x32 drawn here, saved 512x512 (appdata/icon.png, as kode-apps asks).

python3 make_icons.py [outdir]   -> <outdir>/<app>.png (default: icons/)
"""
import os
import sys

from PIL import Image, ImageDraw

N = 32


def hexc(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4)) + (255,)


class P:
    def __init__(self, bg):
        self.im = Image.new("RGBA", (N, N), hexc(bg))
        self.d = ImageDraw.Draw(self.im)

    def px(self, x, y, c):
        if 0 <= x < N and 0 <= y < N:
            self.im.putpixel((x, y), hexc(c))

    def rect(self, x, y, w, h, c):
        self.d.rectangle([x, y, x + w - 1, y + h - 1], fill=hexc(c))

    def disc(self, cx, cy, r, c):
        for y in range(N):
            for x in range(N):
                if (x + .5 - cx) ** 2 + (y + .5 - cy) ** 2 <= r * r:
                    self.px(x, y, c)

    def art(self, rows, x0, y0, pal):
        for j, row in enumerate(rows):
            for i, ch in enumerate(row):
                if ch in pal:
                    self.px(x0 + i, y0 + j, pal[ch])

    def save(self, path):
        self.im.resize((512, 512), Image.NEAREST).save(path)


def slang():
    p = P("#1F4D2B")
    for y in range(0, N, 4):          # grass rows
        for x in range((y // 4) % 2 * 2, N, 4):
            p.px(x, y + 1, "#245A32")
    body = [(6, 22), (7, 22), (8, 22), (9, 22), (10, 22), (11, 22), (12, 22), (12, 21), (12, 20), (12, 19), (12, 18),
            (13, 18), (14, 18), (15, 18), (16, 18), (17, 18), (18, 18), (18, 17), (18, 16), (18, 15), (18, 14),
            (19, 14), (20, 14), (21, 14), (22, 14)]
    for x, y in body:
        p.rect(x, y, 2, 2, "#46BE5A")
    p.rect(22, 12, 4, 4, "#46BE5A")    # head
    p.px(24, 12, "#0B1A10"); p.px(24, 15, "#0B1A10")
    p.px(26, 13, "#E0303D"); p.px(27, 14, "#E0303D")   # tongue
    p.disc(9.5, 9.5, 3.6, "#E0303D")   # apple
    p.px(9, 5, "#5A3A1A"); p.px(10, 5, "#5A3A1A"); p.rect(11, 4, 2, 2, "#46BE5A")
    p.px(8, 8, "#FF8A8A")
    return p


def muurbreker():
    p = P("#16181F")
    cols = ["#C9303D", "#F58228", "#FAC828", "#46BE5A", "#028AC2"]
    for r, c in enumerate(cols):
        for i in range(4):
            if (r, i) in ((3, 2), (4, 1), (4, 3)):
                continue
            p.rect(3 + i * 7, 4 + r * 3, 6, 2, c)
    p.rect(10, 27, 12, 2, "#E8EAED")   # paddle
    p.rect(17, 20, 2, 2, "#FFFFFF")    # ball
    p.px(15, 22, "#5B626D"); p.px(13, 24, "#3A3F47")
    return p


def blokkenregen():
    p = P("#2A1A45")
    for x in range(2, 30):
        p.px(x, 29, "#3D2A60")
    pieces = [("#9658DC", [(4, 25), (8, 25), (12, 25), (12, 21)]),
              ("#3CC8DC", [(16, 25), (20, 25), (24, 25), (24, 21)]),
              ("#FAC828", [(4, 21), (8, 21), (4, 17), (8, 17)]),
              ("#F06EAA", [(16, 21), (20, 21), (20, 17), (24, 17)]),
              ("#46BE5A", [(13, 4), (13, 8), (17, 8), (17, 12)])]
    for c, cells in pieces:
        for x, y in cells:
            p.rect(x, y, 4, 4, c)
            p.px(x, y, "#FFFFFF")
            p.rect(x + 1, y + 3, 3, 1, "#00000055".replace("55", "") if False else c)
    for y in (6, 10, 14):              # falling streaks
        p.px(11, y, "#6A4FA0"); p.px(22, y - 2, "#6A4FA0")
    return p


def schuif():
    p = P("#6B5418")
    p.rect(3, 3, 26, 26, "#8C7224")
    tiles = {(0, 0): "#FFF1C2", (1, 0): "#FAC828", (0, 1): "#F58228", (1, 1): "#C9303D"}
    digits = {"2": ["111", "001", "111", "100", "111"], "4": ["101", "101", "111", "001", "001"],
              "8": ["111", "101", "111", "101", "111"], "6": ["111", "100", "111", "101", "111"]}
    nums = {(0, 0): "2", (1, 0): "4", (0, 1): "8", (1, 1): "6"}
    for (i, j), c in tiles.items():
        x, y = 5 + i * 12, 5 + j * 12
        p.rect(x, y, 10, 10, c)
        rows = digits[nums[(i, j)]]
        ink = "#4A3A10" if j == 0 and i == 0 else "#FFFFFF"
        if (i, j) == (1, 1):
            p.art(["111", "101"], x + 1, y + 2, {})  # noqa
            for k, row in enumerate(digits["1"] if "1" in digits else digits["6"]):
                pass
            # "16": two digits
            p.art([r.replace("1", "a") for r in ["010", "110", "010", "010", "111"]], x + 1, y + 2, {"a": ink})
            p.art([r.replace("1", "a") for r in digits["6"]], x + 5, y + 2, {"a": ink})
        else:
            p.art([r.replace("1", "a") for r in rows], x + 3, y + 2, {"a": ink})
    return p


def fladder():
    p = P("#1D2B4F")
    for x, y in ((3, 4), (9, 2), (27, 3), (20, 7), (6, 13), (29, 12), (14, 27)):
        p.px(x, y, "#E8EAED")
    p.disc(26, 7, 3, "#F2E6B8")                       # moon
    for x, top, bot in ((2, 10, 21), (24, 13, 24)):   # stone columns
        p.rect(x, 0, 6, top, "#8E8AA8"); p.rect(x, bot, 6, N - bot, "#8E8AA8")
        for yy in range(1, top, 3):
            p.rect(x, yy, 6, 1, "#6E6A88")
        for yy in range(bot + 1, N, 3):
            p.rect(x, yy, 6, 1, "#6E6A88")
    blob = ["...cccc...",
            "..cccccc..",
            ".ccwkcwkc.",
            "wccccccccw",
            "wwccccccww",
            ".cccppccc.",
            "..cccccc..",
            "...c..c..."]
    p.art(blob, 11, 12, {"c": "#3CC8DC", "w": "#E8F9FC", "k": "#111111", "p": "#F06EAA"})
    for k in range(3):
        p.px(8 - k * 2, 17 + k, "#5D7BB8")
    return p


def tafeltennis():
    p = P("#0B4F70")
    p.rect(2, 2, 28, 28, "#0E6B97")
    for y in range(3, 29, 3):
        p.rect(15, y, 2, 2, "#CFE8F3")
    p.rect(4, 8, 2, 9, "#FFFFFF")
    p.rect(26, 15, 2, 9, "#FFFFFF")
    p.rect(19, 10, 2, 2, "#FAC828")
    p.px(17, 12, "#4C9CC4"); p.px(15, 14, "#2F84AE")
    return p


def kleurgeheugen():
    p = P("#2A1216")
    q = {(0, 0): "#46BE5A", (1, 0): "#C9303D", (0, 1): "#FAC828", (1, 1): "#028AC2"}
    for y in range(N):
        for x in range(N):
            dx, dy = x + .5 - 16, y + .5 - 16
            r2 = dx * dx + dy * dy
            if 25 <= r2 <= 182:
                c = q[(int(dx > 0), int(dy > 0))]
                if abs(dx) < 1 or abs(dy) < 1:
                    continue
                if (dx > 0) and (dy < 0):
                    c = "#FF5A66"          # the lit one
                p.px(x, y, c)
    p.disc(16, 16, 4.2, "#14090B")
    p.disc(16, 16, 2.2, "#E8EAED")
    return p


def krabbel():
    p = P("#F7D3E4")
    pts = [(4, 22), (6, 18), (8, 16), (10, 17), (11, 20), (13, 22), (15, 20), (16, 16), (17, 13), (19, 12),
           (21, 14)]
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        steps = max(abs(x1 - x0), abs(y1 - y0))
        for t in range(steps + 1):
            x = round(x0 + (x1 - x0) * t / steps); y = round(y0 + (y1 - y0) * t / steps)
            p.rect(x, y, 2, 2, "#C2367A")
    pencil = [(22 + k, 11 - k) for k in range(8)]
    for k, (x, y) in enumerate(pencil):
        p.rect(x, y, 3, 3, "#FAC828" if k > 1 else "#F3C9A0")
    p.rect(21, 13, 2, 2, "#3A3F47")
    p.rect(29, 3, 2, 2, "#F06EAA")
    for x, y in ((6, 6), (9, 4), (5, 27), (25, 25)):
        p.px(x, y, "#F06EAA")
    return p


def stopwatch():
    p = P("#2E3238")
    p.rect(14, 2, 4, 3, "#AEB5BF"); p.rect(12, 1, 8, 2, "#E8EAED")
    p.rect(24, 6, 3, 3, "#AEB5BF")
    p.disc(16, 18, 12, "#E8EAED")
    p.disc(16, 18, 10, "#FFFFFF")
    for a in range(12):
        import math
        x = 16 + math.cos(a * math.pi / 6) * 8.4; y = 18 + math.sin(a * math.pi / 6) * 8.4
        p.px(int(x), int(y), "#8B929C")
    p.rect(15, 11, 2, 8, "#191919")
    for k in range(5):
        p.px(17 + k, 18 + k // 2, "#C9303D")
    p.rect(15, 17, 2, 2, "#C9303D")
    return p


def reflex():
    p = P("#173D22")
    p.disc(16, 16, 13, "#46BE5A")
    p.disc(16, 16, 11, "#2E9E4A")
    bolt = ["......yyy",
            ".....yyy.",
            "....yyy..",
            "...yyy...",
            "..yyyyyy.",
            ".....yyy.",
            "....yyy..",
            "...yyy...",
            "..yy.....",
            ".y......."]
    p.art(bolt, 11, 10, {"y": "#FFE14A"})
    p.art(["......w", ".....w.", "....w..", "...w..."], 11, 10, {"w": "#FFFFFF"})
    return p


ICONS = {"slang": slang, "muurbreker": muurbreker, "blokkenregen": blokkenregen, "schuif": schuif,
         "fladder": fladder, "tafeltennis": tafeltennis, "kleurgeheugen": kleurgeheugen, "krabbel": krabbel,
         "stopwatch": stopwatch, "reflex": reflex}

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "icons")
    os.makedirs(out, exist_ok=True)
    for k, f in ICONS.items():
        f().save(os.path.join(out, k + ".png"))
    sheet = Image.new("RGBA", (5 * 140, 2 * 140), (222, 215, 205, 255))
    for i, k in enumerate(ICONS):
        im = Image.open(os.path.join(out, k + ".png")).resize((128, 128), Image.NEAREST)
        sheet.paste(im, (6 + (i % 5) * 140, 6 + (i // 5) * 140))
    sheet.save(os.path.join(out, "_sheet.png"))
    print("ok", len(ICONS))
