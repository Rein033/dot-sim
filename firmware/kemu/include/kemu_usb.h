/**
 * @file kemu_usb.h
 * @brief The USB-OTG port's end of kemu: registers the Mac's USB frames.
 */
#ifndef KEMU_USB_H_
#define KEMU_USB_H_

/** Take the Mac's USB_BUS, USB_SETUP and USB_OUT frames. Runs before the app's own app_main(). */
void kemu_usb_start(void);

#endif /* KEMU_USB_H_ */
