/**
 * @file kemu_console.h
 * @brief The console's input arrives over UART1 (CONSOLE_IN), not esp-emu's stdin.
 */
#ifndef KEMU_CONSOLE_H_
#define KEMU_CONSOLE_H_

/** Make the buffer and listen for CONSOLE_IN; before the link's rx task runs. */
void kemu_console_start(void);

#endif
