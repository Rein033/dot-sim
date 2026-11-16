/**
 * @file kemu_console.c
 * @brief The console's input, from the Mac over UART1.
 *
 * On the board the console arrives on the USB-Serial/JTAG, and kodeOS reads
 * it with usb_serial_jtag_read_bytes(). Under the emulator that endpoint is
 * fed from esp-emu's stdin, which hands a line over only once its newline
 * arrives: fine for typed commands, no use for the raw bytes of an
 * app_install or a file_put. So the read is wrapped, and its bytes come from
 * CONSOLE_IN frames instead - the bridge's serial port, its page and its
 * terminal all write there.
 *
 * The buffer is the driver's size in kodeOS (KHAL_CONSOLE_RX_BUF, 8 KB):
 * room for the 4 KB chunk a payload sends before waiting for its ack. What
 * does not fit is dropped, as the board's ring drops it.
 */
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#include "kemu_console.h"
#include "kemu_link.h"
#include "kemu_proto.h"

#define CONSOLE_RX 8192U

static StreamBufferHandle_t s_in;

static void on_console_in(const uint8_t *p, uint32_t len)
{
    (void)xStreamBufferSend(s_in, p, len, pdMS_TO_TICKS(100));
}

void kemu_console_start(void)
{
    s_in = xStreamBufferCreate(CONSOLE_RX, 1);
    configASSERT(s_in != NULL);
    kemu_on(KEMU_T_CONSOLE_IN, on_console_in);
}

int __wrap_usb_serial_jtag_read_bytes(void *buf, uint32_t length, uint32_t ticks_to_wait)
{
    if ((buf == NULL) || (length == 0U) || (s_in == NULL)) {
        return 0;
    }
    return (int)xStreamBufferReceive(s_in, buf, length, (TickType_t)ticks_to_wait);
}
