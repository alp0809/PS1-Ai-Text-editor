/*
 * kbtest - tests only the USB keyboard of ps1-writer.
 *
 * Same wiring and USB setup as the real firmware:
 *   keyboard socket: D+ GP12, D- GP13, 5 V from VBUS (pin 40), GND
 *   the Pico's own USB: serial port with the test output (any baud rate)
 * Nothing else needs to be connected (no PS1, no SD card).
 *
 * It prints:
 *   - when something is electrically detected on the keyboard socket
 *   - each USB device (VID:PID) and HID interface (keyboard / mouse / other)
 *   - every key report in hex, and what the ps1-writer keymap makes of it
 *   - each typed line, on Enter
 * LED: slow blink = running, quick flash = key event.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/stdio/driver.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"

#include "tusb.h"
#include "pio_usb.h"
#include "pio_usb_ll.h"

#include "keymap.h"
#include "charset.h"
#include "pw_protocol.h"

#define PIN_USB_DP 12            /* D- = 13 */

/* ---------------------------------------------------------------- output */

static void cdc_out_chars(const char *buf, int len) {
	if (!tud_cdc_connected())
		return;
	int tries = 0;
	while (len > 0) {
		uint32_t n = tud_cdc_write(buf, (uint32_t)len);
		if (n == 0) {
			tud_task();           /* let the PC take what is waiting */
			if (!tud_cdc_connected() || ++tries > 2000)
				return;           /* terminal not reading: drop the rest */
			continue;
		}
		buf += n;
		len -= (int)n;
	}
	tud_cdc_write_flush();
}

static stdio_driver_t cdc_stdio = {
	.out_chars = cdc_out_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
	.crlf_enabled = true,
#endif
};

/* a Latin-1 character as UTF-8 text */
static const char *l1(uint8_t c) {
	static char s[5];
	int n = utf8_encode(latin1_to_unicode(c), (uint8_t *)s);
	s[n] = 0;
	return s;
}

/* ---------------------------------------------------------------- events */

static char     line[256];
static int      line_len;
static uint32_t events, flash_until;

static const char *key_name(uint8_t k) {
	static const char *const names[] = {
		[PW_KEY_UP] = "UP", [PW_KEY_DOWN] = "DOWN",
		[PW_KEY_LEFT] = "LEFT", [PW_KEY_RIGHT] = "RIGHT",
		[PW_KEY_HOME] = "HOME", [PW_KEY_END] = "END",
		[PW_KEY_PGUP] = "PGUP", [PW_KEY_PGDN] = "PGDN",
		[PW_KEY_BACKSPACE] = "BACKSPACE", [PW_KEY_DELETE] = "DELETE",
		[PW_KEY_ENTER] = "ENTER", [PW_KEY_TAB] = "TAB",
		[PW_KEY_ESC] = "ESC", [PW_KEY_INSERT] = "INSERT",
	};
	static char f[4];
	if (k >= PW_KEY_F1 && k < PW_KEY_F1 + 12) {
		snprintf(f, sizeof f, "F%d", k - PW_KEY_F1 + 1);
		return f;
	}
	return (k < sizeof names / sizeof names[0] && names[k]) ? names[k] : "?";
}

static void mods_text(uint8_t m, char *out) {
	out[0] = 0;
	if (m & PW_MOD_CTRL)  strcat(out, "Ctrl+");
	if (m & PW_MOD_SHIFT) strcat(out, "Shift+");
	if (m & PW_MOD_ALT)   strcat(out, "Alt+");
	if (m & PW_MOD_ALTGR) strcat(out, "AltGr+");
}

/* the keymap's output: what the PS1 would receive */
static int on_event(uint8_t kind, uint8_t code, uint8_t mods) {
	char m[32];
	mods_text(mods, m);
	events++;
	flash_until = to_ms_since_boot(get_absolute_time()) + 60;

	if (kind == PW_EV_CHAR) {
		printf("  -> character '%s' (Latin-1 %02X)\n", l1(code), code);
		if (line_len < (int)sizeof line - 4)
			line_len += utf8_encode(latin1_to_unicode(code), (uint8_t *)line + line_len);
	} else if (kind == PW_EV_CTRL) {
		printf("  -> Ctrl+%c\n", code);
	} else {
		printf("  -> key %s%s\n", m, key_name(code));
		if (code == PW_KEY_ENTER) {
			line[line_len] = 0;
			printf("== typed line: \"%s\"\n", line);
			line_len = 0;
		} else if (code == PW_KEY_BACKSPACE && line_len > 0) {
			do line_len--; while (line_len > 0 && (line[line_len] & 0xC0) == 0x80);
		}
	}
	return 1;
}

/* ---------------------------------------------------------------- TinyUSB */

static int keyboards;

void tuh_mount_cb(uint8_t dev_addr) {
	uint16_t vid = 0, pid = 0;
	tuh_vid_pid_get(dev_addr, &vid, &pid);
	printf("USB device ready: address %u, VID:PID = %04X:%04X\n", dev_addr, vid, pid);
}

void tuh_umount_cb(uint8_t dev_addr) {
	printf("USB device removed: address %u\n", dev_addr);
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len) {
	(void)desc_report;
	const uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);
	const char *what = proto == HID_ITF_PROTOCOL_KEYBOARD ? "KEYBOARD" :
	                   proto == HID_ITF_PROTOCOL_MOUSE ? "mouse (ignored)" :
	                   "other HID (ignored)";
	printf("  HID interface %u: %s, report descriptor %u bytes\n", instance, what, desc_len);
	if (proto == HID_ITF_PROTOCOL_KEYBOARD) {
		keyboards++;
		printf("== KEYBOARD READY - try typing (US layout)\n");
	}
	if (!tuh_hid_receive_report(dev_addr, instance))
		printf("  !! could not start reading reports\n");
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
	if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD) {
		if (keyboards > 0) keyboards--;
		keymap_reset();
		printf("== keyboard removed\n");
	}
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len) {
	if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD) {
		printf("report:");
		for (uint16_t i = 0; i < len; i++)
			printf(" %02X", report[i]);
		printf("\n");
		keymap_report(report, len, to_ms_since_boot(get_absolute_time()));
	}
	tuh_hid_receive_report(dev_addr, instance);
}

/* ---------------------------------------------------------------- main */

static uint32_t core1_stack[16 * 1024 / sizeof(uint32_t)];

/* everything on core 1, exactly like the real firmware */
static void core1_main(void) {
	tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO };
	tusb_init(BOARD_TUD_RHPORT, &dev_init);
	stdio_set_driver_enabled(&cdc_stdio, true);

	pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
	pio_cfg.pin_dp = PIN_USB_DP;
	pio_cfg.pio_tx_num = 1;
	pio_cfg.pio_rx_num = 1;
	int tx_ch = dma_claim_unused_channel(true);
	dma_channel_unclaim((uint)tx_ch);       /* PIO-USB claims it itself */
	pio_cfg.tx_ch = (uint8_t)tx_ch;
	tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
	tusb_rhport_init_t host_init = { .role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_AUTO };
	tusb_init(BOARD_TUH_RHPORT, &host_init);

	keymap_init(on_event);

	gpio_init(PICO_DEFAULT_LED_PIN);
	gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

	int was_connected = 0, was_terminal = 0;
	uint32_t next_status = 0, next_blink = 0, connected_at = 0;
	int led = 0;

	for (;;) {
		tud_task();
		tuh_task();
		const uint32_t now = to_ms_since_boot(get_absolute_time());
		keymap_tick(now);

		/* terminal just opened: say hello */
		const int term = tud_cdc_connected();
		if (term && !was_terminal) {
			printf("\n=== ps1-writer keyboard test ===\n");
			printf("Keyboard socket: D+ GP12, D- GP13, 5V = VBUS (pin 40), GND.\n");
			printf("If the keyboard is already plugged in, unplug it and plug it in again.\n\n");
			next_status = now + 1000;
		}
		was_terminal = term;

		/* electrical detection on the socket, before USB enumeration */
		const root_port_t *rp = PIO_USB_ROOT_PORT(0);
		const int conn = rp->initialized && rp->connected;
		if (conn != was_connected) {
			if (conn) {
				printf("Device detected on the socket (%s speed), enumerating...\n",
				       rp->is_fullspeed ? "full" : "low");
				connected_at = now;
			} else {
				printf("Nothing on the socket.\n");
			}
			was_connected = conn;
		}
		if (conn && connected_at && keyboards == 0 && now - connected_at > 4000) {
			printf("!! A device was detected but did not become a keyboard within 4 s.\n"
			       "   D+/D- may be swapped or the wires too long;\n"
			       "   try 15k resistors from D+ and from D- to GND.\n");
			connected_at = 0;
		}

		if (term && now >= next_status) {
			next_status = now + 5000;
			printf("[status] socket: %s, keyboard: %s, events: %lu\n",
			       conn ? "device" : "empty", keyboards ? "ready" : "none",
			       (unsigned long)events);
		}

		/* LED: slow blink, or on briefly after a key event */
		if (now < flash_until) {
			gpio_put(PICO_DEFAULT_LED_PIN, 1);
		} else if (now >= next_blink) {
			led = !led;
			gpio_put(PICO_DEFAULT_LED_PIN, led);
			next_blink = now + 500;
		}
	}
}

int main(void) {
	set_sys_clock_khz(120000, true);     /* PIO-USB needs a multiple of 12 MHz */
	multicore_launch_core1_with_stack(core1_main, core1_stack, sizeof core1_stack);
	for (;;)
		__wfi();
}
