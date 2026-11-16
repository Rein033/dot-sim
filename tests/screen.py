"""The panel as the Mac sees it: FB_RECT frames composed into one picture."""

import struct

import kd

W, H = 410, 502


class Screen:
    def __init__(self) -> None:
        self.pixels = bytearray(W * H * 2)
        self.rects = 0

    def apply(self, payload: bytes) -> None:
        x, y, w, h = struct.unpack_from("<HHHH", payload)
        body = memoryview(payload)[8:]
        for row in range(h):
            dst = ((y + row) * W + x) * 2
            self.pixels[dst:dst + w * 2] = body[row * w * 2:(row + 1) * w * 2]
        self.rects += 1

    def lit_fraction(self) -> float:
        px = memoryview(self.pixels).cast("H")
        return sum(1 for v in px if v) / len(px)

    def take(self, frames) -> None:
        for t, p in frames:
            if t == kd.FB_RECT:
                self.apply(p)
