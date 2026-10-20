/*
 * SD card wiring for the no-OS-FatFS-SD-SDIO-SPI-RPi-Pico library (SPI mode).
 *
 *   SD module   Pico 2
 *   ---------   ------
 *   MISO / DO   GP16
 *   CS          GP17
 *   SCK / CLK   GP18
 *   MOSI / DI   GP19
 *   VCC         3V3(OUT)   (bare card / 3.3V module; a module with a regulator: VBUS)
 *   GND         GND
 */

#include "hw_config.h"

static spi_t spi = {
	.hw_inst   = spi0,
	.miso_gpio = 16,
	.sck_gpio  = 18,
	.mosi_gpio = 19,
	/* 1 MHz works with long breadboard wires (4 MHz did not in the video
	 * project's setup). With short wires try 4 MHz: opening and saving a
	 * big document converts the whole file, so faster is nicer. */
	.baud_rate = 1 * 1000 * 1000,
};

static sd_spi_if_t spi_if = {
	.spi     = &spi,
	.ss_gpio = 17,
};

static sd_card_t sd_card = {
	.type     = SD_IF_SPI,
	.spi_if_p = &spi_if,
};

size_t sd_get_num(void) { return 1; }

sd_card_t *sd_get_by_num(size_t num) {
	return (num == 0) ? &sd_card : NULL;
}
