/*
 * fileserver.h - handles the PS1's file requests (runs on core 1).
 *
 * Folder layout on the SD card:
 *   /ps1writer/            your documents (UTF-8 text, open fine on a PC)
 *   /ps1writer/backup/     the last 5 saved versions of each document
 *   /ps1writer/.work/      the Pico's working files (Latin-1 copy etc.)
 */

#ifndef FILESERVER_H
#define FILESERVER_H

#include <stdint.h>

#define FS_DOC_DIR   "/ps1writer"
#define FS_BAK_DIR   "/ps1writer/backup"
#define FS_WORK_DIR  "/ps1writer/.work"
#define FS_BACKUPS   5
#define FS_MAX_DOC   (1024u * 1024u)   /* 1 MB of text */

void fileserver_init(void);            /* after the card is mounted */

/* Process one request payload, write the response payload.
 * Returns the response length. */
uint16_t fileserver_process(const uint8_t *req, uint16_t len, uint8_t *resp);

/* path of the open document's UTF-8 file; 0 if none is open */
int fileserver_doc_path(char *out, int outsz);

/* Mailbox glue: if core 0 stored a request, process it. Returns 1 if it did. */
int fileserver_poll(void);

#endif
