/**
 * @file kemu_frame.c
 * @brief Frame header and a resyncing decoder (see kemu_proto.h).
 *
 * The decoder is a state machine that needs no more memory than the
 * caller's buffer: until two bytes read 'K' 'D' it is hunting; then it
 * collects the header a byte at a time; then the payload, copied a read's
 * worth at a time. A length that
 * would not fit is taken for corruption, not a frame: nothing the Mac sends
 * is that big. The decoder steps past that 'K' 'D' and hunts again through
 * the header bytes it had already taken.
 */
#include <string.h>

#include "kemu_proto.h"

size_t kemu_frame_header(uint8_t out[KEMU_FRAME_HEADER], uint8_t type,
                         uint32_t len)
{
    out[0] = 'K';
    out[1] = 'D';
    out[2] = type;
    out[3] = (uint8_t)len;
    out[4] = (uint8_t)(len >> 8);
    out[5] = (uint8_t)(len >> 16);
    out[6] = (uint8_t)(len >> 24);
    return KEMU_FRAME_HEADER;
}

void kemu_dec_init(kemu_dec_t *d, uint8_t *buf, size_t cap)
{
    memset(d, 0, sizeof *d);
    d->buf = buf;
    d->cap = cap;
}

int kemu_dec_feed(kemu_dec_t *d, const uint8_t *in, size_t n,
                  kemu_frame_cb_t on_frame, void *arg)
{
    int frames = 0;

    for (size_t i = 0; i < n; i++) {
        uint8_t const b = in[i];

        if (d->have == 0U) {                    /* hunting for 'K' */
            if (b == 'K') {
                d->buf[d->have++] = b;
            }
            continue;
        }
        if (d->have == 1U) {                    /* 'K' seen: want 'D' */
            if (b == 'D') {
                d->buf[d->have++] = b;
            } else {
                d->have = (b == 'K') ? 1U : 0U;
            }
            continue;
        }
        if (d->have < KEMU_FRAME_HEADER) {
            d->buf[d->have++] = b;
            if (d->have == KEMU_FRAME_HEADER) {
                d->len = (uint32_t)d->buf[3] | ((uint32_t)d->buf[4] << 8)
                       | ((uint32_t)d->buf[5] << 16) | ((uint32_t)d->buf[6] << 24);
                /* Not header + len > cap: on a 32-bit size_t that sum wraps
                 * for a length near 4 GB and would let it through. */
                if (d->len > d->cap - KEMU_FRAME_HEADER) {
                    uint8_t rest[KEMU_FRAME_HEADER - 2U];
                    memcpy(rest, d->buf + 2, sizeof rest);
                    d->have = 0U;               /* not a frame: hunt again */
                    frames += kemu_dec_feed(d, rest, sizeof rest, on_frame, arg);
                } else if (d->len == 0U) {
                    on_frame(d->buf[2], d->buf + KEMU_FRAME_HEADER, 0U, arg);
                    frames++;
                    d->have = 0U;
                }
            }
            continue;
        }
        /* The payload: as much of it as this read holds, in one copy. */
        size_t const need = (size_t)KEMU_FRAME_HEADER + d->len - d->have;
        size_t const take = (n - i < need) ? (n - i) : need;
        memcpy(&d->buf[d->have], &in[i], take);
        d->have += take;
        i += take - 1U;
        if (d->have == (size_t)KEMU_FRAME_HEADER + d->len) {
            on_frame(d->buf[2], d->buf + KEMU_FRAME_HEADER, d->len, arg);
            frames++;
            d->have = 0U;
        }
    }
    return frames;
}

static size_t put16(uint8_t *out, uint16_t v)
{
    out[0] = (uint8_t)v;
    out[1] = (uint8_t)(v >> 8);
    return 2U;
}

size_t kemu_pack16(const uint16_t *px, size_t n, uint8_t *out)
{
    size_t o = 0;
    size_t i = 0;
    while (i < n) {
        size_t run = 1;
        while ((i + run < n) && (run < 129U) && (px[i + run] == px[i])) {
            run++;
        }
        if (run >= 2U) {
            out[o++] = (uint8_t)(126U + run);
            o += put16(&out[o], px[i]);
            i += run;
            continue;
        }
        /* Literals, up to where a run of two starts. */
        size_t lit = 1;
        while ((i + lit < n) && (lit < 128U)
               && !((i + lit + 1U < n) && (px[i + lit + 1U] == px[i + lit]))) {
            lit++;
        }
        out[o++] = (uint8_t)(lit - 1U);
        for (size_t k = 0; k < lit; k++) {
            o += put16(&out[o], px[i + k]);
        }
        i += lit;
    }
    return o;
}
