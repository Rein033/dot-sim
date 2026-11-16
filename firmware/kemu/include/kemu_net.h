/**
 * @file kemu_net.h
 * @brief The simulated Kode Dot's network: the P4's emulated Ethernet on esp-emu's user-mode net.
 */
#ifndef KEMU_NET_H_
#define KEMU_NET_H_

#include <stdbool.h>
#include <stdint.h>

/** Called from the event loop's task with the leased address, network byte order. */
typedef void (*kemu_net_cb_t)(uint32_t ip4);

/** Bring the Ethernet up and ask DHCP (once; later calls do nothing). False if it could not. */
bool kemu_net_start(kemu_net_cb_t on_ip);

#endif
