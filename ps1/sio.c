/* sio.c - SIO0 (controller / memory card bus) driver, port 2, polled */

#include <stdint.h>
#include "sio.h"

#define JOY_DATA (*(volatile uint8_t  *)0x1f801040)
#define JOY_STAT (*(volatile uint32_t *)0x1f801044)
#define JOY_MODE (*(volatile uint16_t *)0x1f801048)
#define JOY_CTRL (*(volatile uint16_t *)0x1f80104a)
#define JOY_BAUD (*(volatile uint16_t *)0x1f80104e)
#define IRQ_MASK_REG (*(volatile uint32_t *)0x1f801074)

#define STAT_TX_READY  0x0001
#define STAT_RX_READY  0x0002
#define STAT_ACK_LOW   0x0080
#define STAT_ACK_IRQ   0x0200

#define CTRL_TX_EN     0x0001
#define CTRL_SELECT    0x0002
#define CTRL_IRQ_ACK   0x0010
#define CTRL_RESET     0x0040
#define CTRL_ACK_IRQEN 0x1000
#define CTRL_PORT2     0x2000

/* 33.8688 MHz / 0x88 = ~250 kHz, the stock controller speed */
#ifndef SIO_BAUD_RELOAD
#define SIO_BAUD_RELOAD 0x0088
#endif

static void delay(int n) {
	for (volatile int i = 0; i < n; i++);
}

void sio_init(void) {
	IRQ_MASK_REG &= ~(1 << 7);   /* polled: no interrupt handler steals ACKs */
	JOY_CTRL = CTRL_RESET;
	JOY_MODE = 0x000d;
	JOY_BAUD = SIO_BAUD_RELOAD;
	JOY_CTRL = 0;
}

void sio_select(void) {
	JOY_CTRL = CTRL_TX_EN | CTRL_SELECT | CTRL_ACK_IRQEN | CTRL_PORT2;
	JOY_CTRL |= CTRL_IRQ_ACK;
	delay(100);
}

void sio_deselect(void) {
	JOY_CTRL = 0;
	delay(100);
}

int sio_xfer(uint8_t tx, uint8_t *rx) {
	int timeout;
	while (!(JOY_STAT & STAT_TX_READY));
	JOY_DATA = tx;
	while (!(JOY_STAT & STAT_RX_READY));
	*rx = JOY_DATA;

	timeout = 3000;
	while (!(JOY_STAT & STAT_ACK_IRQ))
		if (--timeout == 0)
			return 0;
	timeout = 3000;
	while (JOY_STAT & STAT_ACK_LOW)
		if (--timeout == 0)
			return 0;
	JOY_CTRL |= CTRL_IRQ_ACK;
	return 1;
}
