/*
 * ai.h - the AI link: passes the PS1's questions, together with the saved
 * document, to a program on the PC (pc/ai_bridge.py) and collects its
 * answer for the PS1 to fetch piece by piece.
 *
 * Runs on the Pico's core 1. The connection itself (aiport_*) is the
 * second USB serial port on the Pico, or a fake one in the PC tests.
 *
 * PC side of the protocol (text header lines, then raw bytes):
 *   Pico -> PC:  "ASK <paragraph> <offset> <doc bytes> <question bytes>\n"
 *                followed by the document (UTF-8) and the question (UTF-8)
 *                "CANCEL\n"
 *   PC -> Pico:  "HELLO <anything>\n"        the bridge is running
 *                "TXT <n>\n" + n bytes       a piece of the answer (UTF-8)
 *                "END\n"                     the answer is complete
 *                "ERR <n>\n" + n bytes       something went wrong (UTF-8)
 */

#ifndef AI_H
#define AI_H

#include <stdint.h>

/* the connection to the PC */
int      aiport_connected(void);                    /* the PC has the port open */
int      aiport_write(const void *p, uint32_t n);   /* 0 = ok; may block briefly */
int      aiport_read(uint8_t *buf, int max);        /* non-blocking, bytes read */

void     ai_init(void);
void     ai_poll(void);                /* call often: reads what the PC sent */
int      ai_ready(void);               /* the PC bridge said HELLO */

/* PW_OP_AI_* requests from the PS1; out[0] = result, returns length */
uint16_t ai_process(uint8_t op, const uint8_t *a, int alen, uint8_t *out);

#endif
