/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdotsim_bridge.c
 * @brief The sim's link to the outside: a plain pthread on a unix socket.
 *
 * Out (sim -> server), binary records: [u8 type][u32 len, little endian][payload]
 *   'F' frame: u16 w, u16 h, then w*h RGB565 pixels (little endian)
 *   'E' LED: r, g, b          'B' brightness: percent
 *   'H' haptic: u32 ms        'T' tone: u32 hz, u32 ms, u8 volume
 *   'P' PCM: u32 rate, then s16 samples      'L' log text      'X' exit: reason
 *   'N' link: one text message for the paired device (the BLE "Kode Link"
 *       on real hardware; the server's hub in the sim)
 * In (server -> sim), text lines:
 *   "b <mask>"            buttons held (KDOTSIM_BTN_* bits)
 *   "t <0|1> <x> <y>"     touch up/down at a screen pixel
 *   "n <text>"            a link message from the paired device or the hub
 *   "m <w> <h> <base64>"  a mirror picture of the paired device's screen (RGB565 LE)
 *   "p <pct> <usb>"       battery charge in percent, USB power attached 0|1
 *   "i <ax> <ay> <az> <gx> <gy> <gz> <mx> <my> <mz> <temp>"
 *                         motion: milli-g, milli-degrees/s, milligauss, tenths of a degree
 *                         (board axes: x right, y toward the top, z out of the glass)
 *   "s <0|1>"             the microSD card out / in (the card is the folder <data>/sd)
 *   "c <text>"            a console line: echoed back as an 'L' record "> <text>" and
 *                         written, with a newline, to the firmware's stdin
 *   "? bat" / "? imu" / "? sd"
 *                         ask what the SDK's drivers read back from the chips (for tests):
 *                         answered as 'L' records "bat <pct> <usb>", "imu <ten values>" and
 *                         "sd <mounted 0|1> <files on the card>"
 *                         (nothing while the drivers are not up yet)
 *
 * Without KODE_SIM_SOCK the sim runs headless; KODE_SIM_SNAP=<file.ppm> then
 * writes the last frame there (at most twice a second) for tests.
 */
#include "kdotsim.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define FRAME_PX (KDOTSIM_W * KDOTSIM_H)
#define MIN_FRAME_NS (1000000000LL / 40)   /* send at most 40 frames a second */

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t s_wlock = PTHREAD_MUTEX_INITIALIZER;   /* one writer on the socket */
static uint16_t s_frame[FRAME_PX];
static bool s_frame_dirty;
static int s_fd = -1;
static atomic_uint s_buttons;
static atomic_bool s_touch_down;
static atomic_uint s_touch_xy;
static atomic_bool s_started;
static char s_data_dir[512];
static int s_stdin_w = -1;   /* write end of the pipe that is the firmware's stdin */

/* Link messages in, until the app takes them. */
#define LINK_SLOTS 32U
#define LINK_LEN 512U
static char s_link[LINK_SLOTS][LINK_LEN];
static unsigned s_link_head, s_link_tail;   /* under s_lock */

/* The latest mirror picture of the paired device. */
#define MIRROR_MAX (KDOTSIM_W * KDOTSIM_H)
static uint16_t s_mirror[MIRROR_MAX];
static uint16_t s_mirror_w, s_mirror_h;
static bool s_mirror_new;

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while ( len > 0U )
    {
        ssize_t const n = send(fd, p, len, MSG_NOSIGNAL);
        if ( n <= 0 )
        {
            if ( (n < 0) && (EINTR == errno) ) { continue; }
            return;
        }
        p += n;
        len -= (size_t)n;
    }
}

/* Small records (everything but frames) wait in a ring for the sender thread:
 * a FreeRTOS task must never block in send(). A full ring drops the record. */
#define RING_BYTES (4U * 1024U * 1024U)
static uint8_t *s_ring;
static size_t s_ring_head, s_ring_tail;   /* bytes in [tail, head) modulo RING_BYTES */

static void ring_put(const void *p, size_t n)
{
    const uint8_t *b = p;
    for ( size_t i = 0U; i < n; ++i )
    {
        s_ring[(s_ring_head + i) % RING_BYTES] = b[i];
    }
    s_ring_head = (s_ring_head + n) % RING_BYTES;
}

static void record(char type, const void *a, size_t alen, const void *b, size_t blen)
{
    if ( (s_fd < 0) || (NULL == s_ring) ) { return; }
    uint8_t hdr[5];
    uint32_t const len = (uint32_t)(alen + blen);
    hdr[0] = (uint8_t)type;
    memcpy(&hdr[1], &len, 4U);
    pthread_mutex_lock(&s_lock);
    size_t const used = (s_ring_head + RING_BYTES - s_ring_tail) % RING_BYTES;
    if ( (used + sizeof(hdr) + len) < (RING_BYTES - 1U) )
    {
        ring_put(hdr, sizeof(hdr));
        if ( alen ) { ring_put(a, alen); }
        if ( blen ) { ring_put(b, blen); }
        pthread_cond_signal(&s_cond);
    }
    pthread_mutex_unlock(&s_lock);
}

/* With s_lock held: move what the ring holds into @p out (up to @p cap). */
static size_t ring_take(uint8_t *out, size_t cap)
{
    size_t n = 0U;
    while ( (s_ring_tail != s_ring_head) && (n < cap) )
    {
        out[n++] = s_ring[s_ring_tail];
        s_ring_tail = (s_ring_tail + 1U) % RING_BYTES;
    }
    return n;
}

static void frame_record(const uint16_t *px)
{
    uint8_t hdr[9];
    uint32_t const len = 4U + (uint32_t)(FRAME_PX * 2U);
    uint16_t const wh[2] = { KDOTSIM_W, KDOTSIM_H };
    hdr[0] = (uint8_t)'F';
    memcpy(&hdr[1], &len, 4U);
    memcpy(&hdr[5], wh, 4U);
    pthread_mutex_lock(&s_wlock);
    write_all(s_fd, hdr, sizeof(hdr));
    write_all(s_fd, px, FRAME_PX * 2U);
    pthread_mutex_unlock(&s_wlock);
}

static void snap_ppm(const char *path, const uint16_t *px)
{
    char tmp[600];
    (void)snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if ( NULL == f ) { return; }
    fprintf(f, "P6\n%u %u\n255\n", KDOTSIM_W, KDOTSIM_H);
    static uint8_t row[KDOTSIM_W * 3U];
    for ( uint32_t y = 0U; y < KDOTSIM_H; ++y )
    {
        for ( uint32_t x = 0U; x < KDOTSIM_W; ++x )
        {
            uint16_t const c = px[y * KDOTSIM_W + x];
            row[x * 3U + 0U] = (uint8_t)(((c >> 11) & 0x1FU) * 255U / 31U);
            row[x * 3U + 1U] = (uint8_t)(((c >> 5) & 0x3FU) * 255U / 63U);
            row[x * 3U + 2U] = (uint8_t)((c & 0x1FU) * 255U / 31U);
        }
        fwrite(row, 1U, sizeof(row), f);
    }
    fclose(f);
    rename(tmp, path);
}

static void *sender(void *arg)
{
    (void)arg;
    static uint16_t copy[FRAME_PX];
    static uint8_t chunk[65536];
    const char *const snap = getenv("KODE_SIM_SNAP");
    int64_t last = 0;
    int64_t last_snap = 0;
    for ( ;; )
    {
        pthread_mutex_lock(&s_lock);
        while ( !s_frame_dirty && ((NULL == s_ring) || (s_ring_head == s_ring_tail)) )
        {
            pthread_cond_wait(&s_cond, &s_lock);
        }
        /* The small records first, in order. */
        size_t n;
        while ( (NULL != s_ring) && ((n = ring_take(chunk, sizeof(chunk))) > 0U) )
        {
            pthread_mutex_unlock(&s_lock);
            pthread_mutex_lock(&s_wlock);
            write_all(s_fd, chunk, n);
            pthread_mutex_unlock(&s_wlock);
            pthread_mutex_lock(&s_lock);
        }
        bool frame = false;
        if ( s_frame_dirty && ((now_ns() - last) >= MIN_FRAME_NS) )
        {
            memcpy(copy, s_frame, sizeof(copy));
            s_frame_dirty = false;
            frame = true;
        }
        bool const pending = s_frame_dirty;
        pthread_mutex_unlock(&s_lock);
        if ( frame )
        {
            last = now_ns();
            if ( s_fd >= 0 )
            {
                frame_record(copy);
            }
            if ( (NULL != snap) && ((last - last_snap) > 500000000LL) )
            {
                snap_ppm(snap, copy);
                last_snap = last;
            }
        }
        else if ( pending )
        {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 2000000 };
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

static size_t b64_decode(const char *in, uint8_t *out, size_t cap)
{
    static int8_t map[256];
    static bool ready;
    if ( !ready )
    {
        memset(map, -1, sizeof(map));
        const char *const a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for ( int i = 0; i < 64; ++i ) { map[(uint8_t)a[i]] = (int8_t)i; }
        ready = true;
    }
    uint32_t acc = 0U;
    int bits = 0;
    size_t n = 0U;
    for ( const char *p = in; *p && (n < cap); ++p )
    {
        int8_t const v = map[(uint8_t)*p];
        if ( v < 0 ) { continue; }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if ( bits >= 8 )
        {
            bits -= 8;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

static void handle_line(char *line)
{
    if ( 'b' == line[0] )
    {
        atomic_store(&s_buttons, (unsigned)strtoul(line + 1, NULL, 10));
        kdotsim_inputs_changed();
    }
    else if ( 't' == line[0] )
    {
        int down = 0, x = 0, y = 0;
        if ( sscanf(line + 1, "%d %d %d", &down, &x, &y) == 3 )
        {
            if ( x < 0 ) { x = 0; }
            if ( y < 0 ) { y = 0; }
            if ( x >= (int)KDOTSIM_W ) { x = (int)KDOTSIM_W - 1; }
            if ( y >= (int)KDOTSIM_H ) { y = (int)KDOTSIM_H - 1; }
            atomic_store(&s_touch_xy, ((unsigned)x << 16) | (unsigned)y);
            atomic_store(&s_touch_down, down != 0);
            kdotsim_inputs_changed();
        }
    }
    else if ( ('n' == line[0]) && (' ' == line[1]) )
    {
        pthread_mutex_lock(&s_lock);
        if ( (s_link_head - s_link_tail) < LINK_SLOTS )
        {
            (void)snprintf(s_link[s_link_head % LINK_SLOTS], LINK_LEN, "%s", line + 2);
            ++s_link_head;
        }
        pthread_mutex_unlock(&s_lock);
        kdotsim_inputs_changed();
    }
    else if ( ('m' == line[0]) && (' ' == line[1]) )
    {
        unsigned w = 0U, h = 0U;
        int off = 0;
        if ( (sscanf(line + 2, "%u %u %n", &w, &h, &off) >= 2) && (w * h <= MIRROR_MAX) && (w > 0U) )
        {
            static uint8_t raw[MIRROR_MAX * 2U];
            size_t const n = b64_decode(line + 2 + off, raw, sizeof(raw));
            if ( n >= (size_t)w * h * 2U )
            {
                pthread_mutex_lock(&s_lock);
                memcpy(s_mirror, raw, (size_t)w * h * 2U);
                s_mirror_w = (uint16_t)w;
                s_mirror_h = (uint16_t)h;
                s_mirror_new = true;
                pthread_mutex_unlock(&s_lock);
            }
        }
    }
    else if ( ('p' == line[0]) && (' ' == line[1]) )
    {
        int pct = 0, usb = 0;
        if ( sscanf(line + 2, "%d %d", &pct, &usb) == 2 )
        {
            kdotsim_battery_set((uint8_t)((pct < 0) ? 0 : ((pct > 100) ? 100 : pct)), usb != 0);
        }
    }
    else if ( ('s' == line[0]) && (' ' == line[1]) )
    {
        kdotsim_sd_set(atoi(line + 2) != 0);
    }
    else if ( ('i' == line[0]) && (' ' == line[1]) )
    {
        int32_t v[KDOTSIM_MOTION_N];
        int off = 0;
        char const *p = line + 2;
        unsigned n = 0U;
        while ( (n < KDOTSIM_MOTION_N) && (sscanf(p, "%d%n", &v[n], &off) == 1) )
        {
            p += off;
            ++n;
        }
        if ( KDOTSIM_MOTION_N == n )
        {
            kdotsim_motion_set(v);
        }
    }
    else if ( ('c' == line[0]) && (' ' == line[1]) )
    {
        char echo[LINK_LEN + 4U];
        int const n = snprintf(echo, sizeof(echo), "> %s\n", line + 2);
        kdotsim_log(echo, (size_t)((n < (int)sizeof(echo)) ? n : (int)sizeof(echo) - 1));
        if ( s_stdin_w >= 0 )
        {
            char in[LINK_LEN + 2U];
            int const m = snprintf(in, sizeof(in), "%s\n", line + 2);
            if ( m > 0 )
            {
                (void)!write(s_stdin_w, in, (size_t)((m < (int)sizeof(in)) ? m : (int)sizeof(in) - 1));
            }
        }
    }
    else if ( ('?' == line[0]) && (' ' == line[1]) )
    {
        char out[160];
        int const n = kdotsim_probe(line + 2, out, sizeof(out));
        if ( n > 0 )
        {
            kdotsim_log(out, (size_t)n);
        }
    }
    else if ( 'q' == line[0] )
    {
        kdotsim_exit("stopped");
    }
}

static void *reader(void *arg)
{
    (void)arg;
    /* Big enough for a mirror line (base64 of a whole frame). */
    size_t const cap = 1U << 20;
    char *const buf = malloc(cap);
    size_t have = 0U;
    for ( ;; )
    {
        ssize_t const n = recv(s_fd, buf + have, cap - 1U - have, 0);
        if ( n <= 0 )
        {
            if ( (n < 0) && (EINTR == errno) ) { continue; }
            /* The server went away: nobody can see or play the app. */
            _exit(0);
        }
        have += (size_t)n;
        buf[have] = '\0';
        char *start = buf;
        char *nl;
        while ( (nl = strchr(start, '\n')) != NULL )
        {
            *nl = '\0';
            handle_line(start);
            start = nl + 1;
        }
        have = strlen(start);
        memmove(buf, start, have);
        if ( have >= cap - 1U ) { have = 0U; }
    }
    return NULL;
}

const char *kdotsim_data_dir(void)
{
    if ( '\0' == s_data_dir[0] )
    {
        const char *d = getenv("KODE_SIM_DATA");
        (void)snprintf(s_data_dir, sizeof(s_data_dir), "%s", (NULL != d) ? d : "./simdata");
        (void)mkdir(s_data_dir, 0755);
    }
    return s_data_dir;
}

void kdotsim_start(void)
{
    if ( atomic_exchange(&s_started, true) ) { return; }
    (void)kdotsim_data_dir();
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* The FreeRTOS port drives its tick with signals: none may land on these
     * threads, which the port does not know. They inherit this mask. */
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    const char *const path = getenv("KODE_SIM_SOCK");
    if ( NULL != path )
    {
        int const fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un sa = { .sun_family = AF_UNIX };
        (void)snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
        if ( (fd >= 0) && (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) )
        {
            s_fd = fd;
            s_ring = malloc(RING_BYTES);
            /* Console lines from the page arrive on stdin, as from a terminal. */
            int pf[2];
            signal(SIGPIPE, SIG_IGN);   /* a closed stdin must not kill the sim */
            if ( pipe(pf) == 0 )
            {
                if ( 0 != pf[0] )   /* (0 means stdin was closed and is now the pipe already) */
                {
                    (void)dup2(pf[0], 0);
                    (void)close(pf[0]);
                }
                s_stdin_w = pf[1];
                (void)fcntl(s_stdin_w, F_SETFL, fcntl(s_stdin_w, F_GETFL) | O_NONBLOCK);
            }
            pthread_t t;
            (void)pthread_create(&t, NULL, reader, NULL);
            (void)pthread_detach(t);
        }
        else
        {
            fprintf(stderr, "kdotsim: cannot reach %s: %s\n", path, strerror(errno));
            _exit(2);
        }
    }
    pthread_t t;
    (void)pthread_create(&t, NULL, sender, NULL);
    (void)pthread_detach(t);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

/* Before main(): the bridge must exist before FreeRTOS starts. */
__attribute__((constructor)) static void kdotsim_ctor(void)
{
    kdotsim_start();
}

uint32_t kdotsim_buttons(void)
{
    return atomic_load(&s_buttons);
}

bool kdotsim_touch(uint16_t *x, uint16_t *y)
{
    unsigned const xy = atomic_load(&s_touch_xy);
    if ( NULL != x ) { *x = (uint16_t)(xy >> 16); }
    if ( NULL != y ) { *y = (uint16_t)(xy & 0xFFFFU); }
    return atomic_load(&s_touch_down);
}

void kdotsim_frame(const uint16_t *px, uint32_t stride_px, int32_t y1, int32_t y2)
{
    if ( NULL == px ) { return; }
    if ( y1 < 0 ) { y1 = 0; }
    if ( y2 > (int32_t)KDOTSIM_H ) { y2 = (int32_t)KDOTSIM_H; }
    pthread_mutex_lock(&s_lock);
    for ( int32_t y = y1; y < y2; ++y )
    {
        memcpy(&s_frame[(uint32_t)y * KDOTSIM_W], &px[(uint32_t)y * stride_px], KDOTSIM_W * 2U);
    }
    s_frame_dirty = true;
    pthread_cond_signal(&s_cond);
    pthread_mutex_unlock(&s_lock);
}

void kdotsim_led(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t const rgb[3] = { r, g, b };
    record('E', rgb, 3U, NULL, 0U);
}

void kdotsim_brightness(uint8_t percent)
{
    record('B', &percent, 1U, NULL, 0U);
}

void kdotsim_haptic(uint32_t ms)
{
    record('H', &ms, 4U, NULL, 0U);
}

void kdotsim_tone(uint32_t hz, uint32_t ms, uint8_t volume)
{
    uint8_t p[9];
    memcpy(&p[0], &hz, 4U);
    memcpy(&p[4], &ms, 4U);
    p[8] = volume;
    record('T', p, sizeof(p), NULL, 0U);
}

void kdotsim_pcm(const int16_t *samples, size_t n, uint32_t rate)
{
    record('P', &rate, 4U, samples, n * sizeof(int16_t));
}

void kdotsim_log(const char *line, size_t len)
{
    record('L', line, len, NULL, 0U);
}

void kdotsim_exit(const char *why)
{
    const char *const w = (NULL != why) ? why : "exit";
    fprintf(stderr, "kdotsim: %s\n", w);
    record('X', w, strlen(w), NULL, 0U);
    fflush(stdout);
    for ( int i = 0; (i < 50) && (NULL != s_ring) && (s_ring_head != s_ring_tail); ++i )
    {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
        nanosleep(&ts, NULL);
    }
    _exit(0);
}

void kdotsim_link_send(const char *msg, size_t len)
{
    if ( len > LINK_LEN - 1U ) { len = LINK_LEN - 1U; }
    record('N', msg, len, NULL, 0U);
}

int kdotsim_link_recv(char *buf, size_t cap)
{
    int n = -1;
    pthread_mutex_lock(&s_lock);
    if ( s_link_tail != s_link_head )
    {
        n = snprintf(buf, cap, "%s", s_link[s_link_tail % LINK_SLOTS]);
        ++s_link_tail;
    }
    pthread_mutex_unlock(&s_lock);
    return n;
}

bool kdotsim_mirror(uint16_t *dst, uint16_t *w, uint16_t *h, size_t cap_px)
{
    bool fresh = false;
    pthread_mutex_lock(&s_lock);
    if ( s_mirror_new && ((size_t)s_mirror_w * s_mirror_h <= cap_px) )
    {
        memcpy(dst, s_mirror, (size_t)s_mirror_w * s_mirror_h * 2U);
        *w = s_mirror_w;
        *h = s_mirror_h;
        s_mirror_new = false;
        fresh = true;
    }
    pthread_mutex_unlock(&s_lock);
    return fresh;
}
