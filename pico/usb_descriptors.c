/* usb_descriptors.c - the Pico's own USB port shows up on the PC as two
 * serial ports: "ps1-writer log" (messages) and "ps1-writer AI" (the link
 * pc/ai_bridge.py talks to). */

#include <string.h>
#include "tusb.h"
#include "pico/unique_id.h"

#define USB_VID 0xCafe       /* TinyUSB's example vendor id, fine for hobby use */
#define USB_PID 0x4021
#define USB_BCD 0x0200

static const tusb_desc_device_t desc_device = {
	.bLength            = sizeof(tusb_desc_device_t),
	.bDescriptorType    = TUSB_DESC_DEVICE,
	.bcdUSB             = USB_BCD,
	.bDeviceClass       = TUSB_CLASS_MISC,
	.bDeviceSubClass    = MISC_SUBCLASS_COMMON,
	.bDeviceProtocol    = MISC_PROTOCOL_IAD,
	.bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
	.idVendor           = USB_VID,
	.idProduct          = USB_PID,
	.bcdDevice          = 0x0100,
	.iManufacturer      = 1,
	.iProduct           = 2,
	.iSerialNumber      = 3,
	.bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) {
	return (uint8_t const *)&desc_device;
}

enum { ITF_LOG = 0, ITF_LOG_DATA, ITF_AI, ITF_AI_DATA, ITF_NUM_TOTAL };

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + 2 * TUD_CDC_DESC_LEN)

static const uint8_t desc_configuration[] = {
	TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
	/* interface, string, notify ep, size, data out ep, data in ep, size */
	TUD_CDC_DESCRIPTOR(ITF_LOG, 4, 0x81, 8, 0x02, 0x82, 64),
	TUD_CDC_DESCRIPTOR(ITF_AI,  5, 0x83, 8, 0x04, 0x84, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
	(void)index;
	return desc_configuration;
}

static const char *const strings[] = {
	(const char[]){ 0x09, 0x04 },   /* English */
	"ps1-writer",
	"ps1-writer log",
	NULL,                           /* serial: from the chip id */
	"ps1-writer log",
	"ps1-writer AI",
};

static uint16_t desc_str[33];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
	(void)langid;
	size_t n;
	if (index == 0) {
		memcpy(&desc_str[1], strings[0], 2);
		n = 1;
	} else {
		char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
		const char *s;
		if (index == 3) {
			pico_get_unique_board_id_string(serial, sizeof serial);
			s = serial;
		} else if (index < sizeof strings / sizeof strings[0]) {
			s = strings[index];
		} else {
			return NULL;
		}
		n = strlen(s);
		if (n > 32)
			n = 32;
		for (size_t i = 0; i < n; i++)
			desc_str[1 + i] = (uint8_t)s[i];
	}
	desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
	return desc_str;
}
