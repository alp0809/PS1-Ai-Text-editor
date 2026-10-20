/*
 * sio.h - the controller-port bus, as seen by the PS1 program.
 * sio.c implements it on the PS1 hardware; the PC tests implement it by
 * calling the Pico's bus logic directly.
 */

#ifndef SIO_H
#define SIO_H

#include <stdint.h>

void sio_init(void);
void sio_select(void);       /* ATT low on port 2 */
void sio_deselect(void);     /* ATT high */

/* exchange one byte; returns 1 if the device ACKed it */
int  sio_xfer(uint8_t tx, uint8_t *rx);

#endif
