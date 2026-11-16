/**
 * @file kemu_clock.h
 * @brief The P4's RTC, set from the Mac's clock at every boot.
 */
#ifndef KEMU_CLOCK_H_
#define KEMU_CLOCK_H_

#include <stdint.h>

/** Take the Mac's TIME frame; call before kemu_link_start() says HELLO. */
void kemu_clock_start(void);

/** Wait up to @p ms for the Mac's time, so the app reads a set clock. */
void kemu_clock_wait(uint32_t ms);

#endif
