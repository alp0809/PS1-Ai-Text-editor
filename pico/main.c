/*
 * ps1-writer - Pico 2 firmware
 *
 *   core 0: PS1 controller-port bus (PIO + bus_logic.c), nothing else
 *   core 1: USB keyboard (PIO-USB host), the Pico's own USB (two serial
 *           ports: log, and the AI link to the PC), SD card, the PS1's
 *           file and AI requests
 *
 * Wiring (GPIO numbers):
 *   GP2  PS1 CMD      GP3  PS1 CLK     GP4  PS1 ATT
 *   GP5  PS1 DATA     GP6  PS1 ACK
 *   GP12 USB keyboard D+   GP13 USB keyboard D-   (5 V from VBUS, GND)
 *   GP16 SD MISO   GP17 SD CS   GP18 SD SCK   GP19 SD MOSI
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/stdio/driver.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"

#include "tusb.h"
#include "pio_usb.h"

#include "sio0_dev.pio.h"
#include "bus_logic.h"
#include "shared.h"
#include "keymap.h"
#include "fileserver.h"
#include "ai.h"

#define PIN_CMD      2
#define PIN_CLK      3      /* must be PIN_CMD + 1 */
#define PIN_ATT      4
#define PIN_DATA     5
#define PIN_ACK      6
#define PIN_USB_DP   12     /* D- = 13 */

#define CDC_LOG      0      /* the Pico's USB serial ports */
#define CDC_AI       1

#define SM_CLOCK_HZ  50000000.0f

int vfs_mount(void);         /* vfs_fatfs.c */

/* ======================================================================
 * core 0: the PS1 bus
 * ====================================================================== */

static PIO  bus_pio = pio0;  /* PIO-USB uses pio1 */
static uint bus_sm, bus_offset;

static void bus_hw_init(void) {
	const uint inputs[] = { PIN_CMD, PIN_CLK, PIN_ATT };
	for (unsigned i = 0; i < 3; i++) {
		gpio_init(inputs[i]);
		gpio_set_dir(inputs[i], GPIO_IN);
		gpio_disable_pulls(inputs[i]);
	}
	pio_gpio_init(bus_pio, PIN_DATA);
	pio_gpio_init(bus_pio, PIN_ACK);
	gpio_disable_pulls(PIN_DATA);
	gpio_disable_pulls(PIN_ACK);

	bus_offset = pio_add_program(bus_pio, &sio0_dev_program);
	bus_sm = pio_claim_unused_sm(bus_pio, true);

	pio_sm_config c = sio0_dev_program_get_default_config(bus_offset);
	sm_config_set_in_pins(&c, PIN_CMD);
	sm_config_set_out_pins(&c, PIN_DATA, 1);
	sm_config_set_set_pins(&c, PIN_DATA, 1);
	sm_config_set_sideset_pins(&c, PIN_ACK);
	sm_config_set_in_shift(&c, true, false, 32);
	sm_config_set_out_shift(&c, true, false, 32);
	sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / SM_CLOCK_HZ);

	const uint32_t od = (1u << PIN_DATA) | (1u << PIN_ACK);
	pio_sm_set_pins_with_mask(bus_pio, bus_sm, 0, od);
	pio_sm_set_pindirs_with_mask(bus_pio, bus_sm, 0, od);
	pio_sm_init(bus_pio, bus_sm, bus_offset, &c);
}

static void __no_inline_not_in_flash_func(bus_hw_reset)(void) {
	pio_sm_set_enabled(bus_pio, bus_sm, false);
	pio_sm_clear_fifos(bus_pio, bus_sm);
	pio_sm_restart(bus_pio, bus_sm);
	/* release DATA (set pins) and ACK (side-set) with two instructions;
	 * these helpers are inline, so nothing here runs from flash */
	pio_sm_exec(bus_pio, bus_sm, pio_encode_set(pio_pindirs, 0));
	pio_sm_exec(bus_pio, bus_sm, pio_encode_nop() | pio_encode_sideset_opt(1, 0));
	pio_sm_exec(bus_pio, bus_sm, pio_encode_jmp(bus_offset));
}

static void __no_inline_not_in_flash_func(bus_loop)(void) {
	static bus_t b;
	for (;;) {
		while (gpio_get(PIN_ATT))
			tight_loop_contents();

		uint8_t reply = bus_begin(&b);
		pio_sm_put_blocking(bus_pio, bus_sm, (uint8_t)~reply);
		pio_sm_set_enabled(bus_pio, bus_sm, true);

		for (;;) {
			bool selected = !gpio_get(PIN_ATT);
			if (pio_sm_is_rx_fifo_empty(bus_pio, bus_sm)) {
				if (!selected)
					break;
				continue;
			}
			uint8_t rx = (uint8_t)(pio_sm_get(bus_pio, bus_sm) >> 24);
			int ack = bus_byte(&b, rx, &reply);
			pio_sm_put_blocking(bus_pio, bus_sm, ack ? 1u : 0u);
			pio_sm_put_blocking(bus_pio, bus_sm, (uint8_t)~reply);
		}
		bus_end(&b);
		bus_hw_reset();
	}
}

/* ======================================================================
 * core 1: USB, SD card, files
 * ====================================================================== */

/* keep USB and the keyboard going (also while a long transfer waits) */
static void usb_service(void) {
	tud_task();
	tuh_task();
	keymap_tick(to_ms_since_boot(get_absolute_time()));
}

/* printf -> the first USB serial port (only called on core 1) */
static void cdc_out_chars(const char *buf, int len) {
	if (!tud_cdc_n_connected(CDC_LOG))
		return;
	while (len > 0) {
		uint32_t n = tud_cdc_n_write(CDC_LOG, buf, (uint32_t)len);
		if (n == 0)
			break;                /* buffer full: drop the rest */
		buf += n;
		len -= (int)n;
	}
	tud_cdc_n_write_flush(CDC_LOG);
}

/* the AI link: the second USB serial port (see ai.h) */
int aiport_connected(void) {
	return tud_cdc_n_connected(CDC_AI);
}

int aiport_write(const void *p, uint32_t n) {
	const uint8_t *b = p;
	absolute_time_t give_up = make_timeout_time_ms(3000);
	while (n > 0) {
		if (!tud_cdc_n_connected(CDC_AI) || time_reached(give_up))
			return -1;            /* PC gone or not reading */
		uint32_t k = tud_cdc_n_write(CDC_AI, b, n);
		if (k == 0) {
			tud_cdc_n_write_flush(CDC_AI);
			usb_service();
			continue;
		}
		b += k;
		n -= k;
		give_up = make_timeout_time_ms(3000);
	}
	tud_cdc_n_write_flush(CDC_AI);
	return 0;
}

int aiport_read(uint8_t *buf, int max) {
	if (!tud_cdc_n_available(CDC_AI))
		return 0;
	return (int)tud_cdc_n_read(CDC_AI, buf, (uint32_t)max);
}

static stdio_driver_t cdc_stdio = {
	.out_chars = cdc_out_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
	.crlf_enabled = true,
#endif
};

/* USB, FatFs and the file server need more than the default 2 KB */
static uint32_t core1_stack[16 * 1024 / sizeof(uint32_t)];

static void core1_main(void) {
	/* log serial port on the Pico's own USB */
	tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO };
	tusb_init(BOARD_TUD_RHPORT, &dev_init);
	stdio_set_driver_enabled(&cdc_stdio, true);

	/* keyboard on GPIO 12/13 through PIO-USB (pio1, all three of its SMs) */
	pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
	pio_cfg.pin_dp = PIN_USB_DP;
	pio_cfg.pio_tx_num = 1;
	pio_cfg.pio_rx_num = 1;
	/* PIO-USB claims the DMA channel itself (claiming it twice panics), so
	 * only look for a free one here */
	int tx_ch = dma_claim_unused_channel(true);
	dma_channel_unclaim((uint)tx_ch);
	pio_cfg.tx_ch = (uint8_t)tx_ch;
	tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
	tusb_rhport_init_t host_init = { .role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_AUTO };
	tusb_init(BOARD_TUH_RHPORT, &host_init);

	keymap_init(keyq_push);
	ai_init();

	gpio_init(PICO_DEFAULT_LED_PIN);
	gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

	absolute_time_t next_mount = make_timeout_time_ms(1000);   /* card power-up */
	absolute_time_t next_status = make_timeout_time_ms(3000);
	absolute_time_t next_blink = make_timeout_time_ms(500);
	int mount_tries = 0;

	for (;;) {
		usb_service();
		ai_poll();
		if (ai_ready())
			pw_flags |= PW_FLAG_AI;
		else
			pw_flags &= (uint8_t)~PW_FLAG_AI;

		if (!(pw_flags & PW_FLAG_SD) && time_reached(next_mount)) {
			mount_tries++;
			if (vfs_mount()) {
				fileserver_init();
				pw_flags |= PW_FLAG_SD;
				printf("SD card ok\n");
			} else {
				if (mount_tries % 10 == 1)
					printf("SD card not ready yet (attempt %d)\n", mount_tries);
				next_mount = make_timeout_time_ms(500);
			}
		}

		/* LED: slow blink = firmware running, flicker = file requests */
		if (fileserver_poll() || time_reached(next_blink)) {
			gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
			next_blink = make_timeout_time_ms(500);
		}

		if (time_reached(next_status)) {
			next_status = make_timeout_time_ms(5000);
			printf("status: SD %s, keyboard %s, AI bridge %s\n",
			       (pw_flags & PW_FLAG_SD) ? "ok" : "missing",
			       (pw_flags & PW_FLAG_KEYBOARD) ? "connected" : "none",
			       (pw_flags & PW_FLAG_AI) ? "connected" : "not running");
		}
	}
}

int main(void) {
	/* PIO-USB needs a clock that is a multiple of 12 MHz */
	set_sys_clock_khz(120000, true);
	shared_reset();
	bus_hw_init();
	multicore_launch_core1_with_stack(core1_main, core1_stack, sizeof core1_stack);
	bus_loop();
}
