/**
 * @file kemu_sd.h
 * @brief The microSD slot: ESP-IDF's SDMMC host answered by kemu's card, blocks on the Mac.
 */
#ifndef KEMU_SD_H_
#define KEMU_SD_H_

/** Listen for SD_CARD and SD_DATA; before the link's rx task runs. */
void kemu_sd_start(void);

#endif
