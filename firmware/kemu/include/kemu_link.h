/**
 * @file kemu_link.h
 * @brief The firmware's end of the wire to the Mac: UART1, framed.
 *
 * The emulator bridges UART1 to a TCP socket (--uart1-tcp) and ignores the
 * baud rate, so this is fast enough for whole frames of the panel.
 */
#ifndef KEMU_LINK_H_
#define KEMU_LINK_H_

#include <stdint.h>

#include "kemu_proto.h"

typedef void (*kemu_rx_cb_t)(const uint8_t *p, uint32_t len);

/** Bring the link up and say HELLO. Runs before the app's own app_main(). */
void kemu_link_start(void);

/** Send one frame. Thread-safe; blocks until it has left. */
void kemu_send(uint8_t type, const void *p, uint32_t len);

/** Send one frame whose payload is a small header followed by a body. */
void kemu_send2(uint8_t type, const void *head, uint32_t head_len,
                const void *body, uint32_t body_len);

/** The one handler for frames of @p type from the Mac; runs on the link's task. */
void kemu_on(uint8_t type, kemu_rx_cb_t cb);

#endif /* KEMU_LINK_H_ */
