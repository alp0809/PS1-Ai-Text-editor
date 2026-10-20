/*
 * doc.h - the document in PS1 memory: a list of paragraphs (the text
 * between two newlines), Latin-1 bytes, no '\n' inside a paragraph.
 *
 * Every paragraph remembers where its text sits in the Pico's working copy
 * (the last loaded or saved version). Saving sends "keep bytes x..y" for
 * paragraphs that did not change and the text only for those that did, so
 * saving a 300 KB thesis after typing a sentence moves a few hundred bytes.
 */

#ifndef DOC_H
#define DOC_H

#include <stdint.h>
#include "pw_protocol.h"

#define DOC_MAX_BYTES  (1024u * 1024u)   /* same limit as the Pico */

typedef struct {
	uint8_t *t;
	int      len, cap;
	uint32_t ooff;          /* origin: offset in the working copy */
	int      olen;
	uint8_t  clean;         /* text == working copy [ooff, ooff+olen) */
	uint8_t  onl;           /* the working copy has '\n' at ooff+olen */
	uint8_t  nonl;          /* while saving: '\n' after it in the new copy */
	uint16_t words;
	uint32_t sgen;          /* save that last saw it unchanged (0 = none) */
	uint32_t nooff;         /* while saving: offset in the new copy */
} para_t;

typedef struct {
	para_t  *p;
	int      n, cap;
	uint32_t bytes;         /* file size: text + newlines */
	uint32_t words;
	uint8_t  name[PW_MAX_NAME + 1];   /* Latin-1, 0-terminated */
	int      modified;
	uint32_t edit_gen;      /* counts edits */
	uint32_t save_gen;      /* counts saves */
} doc_t;

/* progress: done/total of the current load or save (any unit) */
typedef void (*doc_progress_fn)(uint32_t done, uint32_t total);

void doc_init(doc_t *d);
void doc_clear(doc_t *d);   /* one empty paragraph */
void doc_free(doc_t *d);    /* release everything */

/* editing; all return 0, or -1 if out of memory / document too big */
int  doc_insert(doc_t *d, int pi, int off, const uint8_t *s, int n);
void doc_erase(doc_t *d, int pi, int off, int n);
int  doc_split(doc_t *d, int pi, int off);    /* Enter */
int  doc_join(doc_t *d, int pi);              /* pi + pi+1 */

/* Pico operations: return PW_OK (0), PW_ERR_* or LINK_ERR_* */
int  doc_open(doc_t *d, const uint8_t *name, int nlen, doc_progress_fn prog);
int  doc_create(doc_t *d, const uint8_t *name, int nlen);
/* Saving: the changes are collected first, then sent. While they are sent
 * and the Pico writes the card, idle() is called regularly and may edit the
 * document (typing goes on during a save); such edits stay unsaved. A full
 * save (rare: after a verify failure or a Pico restart) does not call idle. */
int  doc_save(doc_t *d, doc_progress_fn prog, void (*idle)(void));

/* statistics of the last save, for tests and the curious */
extern uint32_t doc_save_sent, doc_save_full;

int  doc_count_words(const uint8_t *t, int len);

#endif
