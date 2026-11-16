/**
 * @file kemu_radio.h
 * @brief The radio's requests without a C5: WiFi over kemu_net, or a C5 that never comes up.
 */
#ifndef KEMU_RADIO_H_
#define KEMU_RADIO_H_

/** Listen for the Mac's RADIO frame; before the link's rx task runs. */
void kemu_radio_start(void);

#endif
