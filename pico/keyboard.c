/* keyboard.c - TinyUSB host callbacks: USB keyboard -> keymap -> key queue */

#include <stdio.h>
#include "tusb.h"
#include "pico/time.h"
#include "keymap.h"
#include "shared.h"

static int keyboards;          /* keyboard interfaces currently mounted */

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len) {
	(void)desc_report;
	(void)desc_len;
	if (tuh_hid_interface_protocol(dev_addr, instance) != HID_ITF_PROTOCOL_KEYBOARD)
		return;                 /* mice, media keys etc. are ignored */
	keyboards++;
	pw_flags |= PW_FLAG_KEYBOARD;
	printf("keyboard connected (device %u)\n", dev_addr);
	if (!tuh_hid_receive_report(dev_addr, instance))
		printf("keyboard: could not start reading\n");
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
	if (tuh_hid_interface_protocol(dev_addr, instance) != HID_ITF_PROTOCOL_KEYBOARD)
		return;
	if (keyboards > 0)
		keyboards--;
	if (keyboards == 0) {
		pw_flags &= (uint8_t)~PW_FLAG_KEYBOARD;
		keymap_reset();
	}
	printf("keyboard disconnected\n");
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len) {
	if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD)
		keymap_report(report, len, now_ms());
	tuh_hid_receive_report(dev_addr, instance);
}
