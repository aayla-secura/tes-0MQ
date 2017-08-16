/*
 * An opaque class for managing the ring structure. At the moment it's just
 * a wrapper around netmap.
 */

#ifndef __NET_FPGAIF_MANAGER_H_INCLUDED__
#define __NET_FPGAIF_MANAGER_H_INCLUDED__

#include "net/fpgaif_reader.h"
#include "sys/types.h"

/* Open or close an interface. */
ifdesc* if_open (const char *name, const ifreq *req,
	uint64_t flags, const ifdesc *arg);
int if_close (ifdesc* ifd);

/* Get the file descriptor */
int if_fd (ifdesc* ifd);

/* Set and get the first, next, <idx> or last tx or rx ring.
 * It is not done in a circular fashion.
 * Returns NULL for rings beyond the last one. */
ifring* if_rewind_txring (ifdesc* ifd);
ifring* if_next_txring (ifdesc* ifd);
ifring* if_goto_txring (ifdesc* ifd, uint16_t idx);
ifring* if_goto_last_txring (ifdesc* ifd);
ifring* if_rewind_rxring (ifdesc* ifd);
ifring* if_next_rxring (ifdesc* ifd);
ifring* if_goto_rxring (ifdesc* ifd, uint16_t idx);
ifring* if_goto_last_rxring (ifdesc* ifd);

/* Set and get the current buffer of a ring to head, next, <idx> or tail-1. */
char* ifring_rewind_buf (ifring* ring);
char* ifring_next_buf (ifring* ring);
char* ifring_goto_buf (ifring* ring, uint32_t idx);
char* ifring_goto_last_buf (ifring* ring);

/* Set and get the head buffer of a ring to next, <idx> or cur. */
char* ifring_release_one_buf (ifring* ring);
char* ifring_release_to_buf (ifring* ring, uint32_t idx);
char* ifring_release_done_buf (ifring* ring);

/* Set the current buffer of a ring to tail+num. */
void ifring_wait_for_more (ifring* ring, uint32_t num);

/* Set both the head and cursor to tail. */
void ifring_release_all (ifring* ring);

/* Same as nm_inject. */
int if_inject (ifdesc* ifd, const void* buf, size_t len);

/* Same as nm_dispatch. */
int if_dispatch (ifdesc* ifd, int cnt, ifpkt_hn handler,
	unsigned char* arg);

#endif
