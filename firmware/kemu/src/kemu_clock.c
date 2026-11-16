/**
 * @file kemu_clock.c
 * @brief The P4's RTC, set from the Mac's clock at every boot.
 *
 * On the board the wall clock lives in the P4's RTC, which keeps it across
 * every reset that leaves the chip powered, and SNTP sets it once the radio
 * is up. esp-emu's RTC starts at zero, and a hosted Kode Dot has no network
 * out to ask - so every boot would stop at "Set the time". The Mac answers
 * HELLO with its time instead (TIME), and kemu puts it where an RTC that
 * kept its supply would have it, before app_main can read it: kaos's boot
 * then finds the clock known, as on a board that was never switched off.
 */
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "kemu_clock.h"
#include "kemu_link.h"
#include "kemu_proto.h"

static SemaphoreHandle_t s_set;

static void on_time(const uint8_t *p, uint32_t len)
{
    uint64_t unix_us;
    if (len < 8U) {
        return;
    }
    memcpy(&unix_us, p, 8);
    const struct timeval tv = {
        .tv_sec  = (time_t)(unix_us / 1000000ULL),
        .tv_usec = (suseconds_t)(unix_us % 1000000ULL),
    };
    (void)settimeofday(&tv, NULL);
    xSemaphoreGive(s_set);
}

void kemu_clock_start(void)
{
    s_set = xSemaphoreCreateBinary();
    configASSERT(s_set != NULL);
    kemu_on(KEMU_T_TIME, on_time);
}

void kemu_clock_wait(uint32_t ms)
{
    (void)xSemaphoreTake(s_set, pdMS_TO_TICKS(ms));   /* no Mac time: the RTC's, as it was */
}
