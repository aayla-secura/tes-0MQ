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

/* Set the current tx or rx ring to first, next, last or <idx>.
 * Returns 0 on succes, -1 on error. */
int if_reset_cur_txring (ifdesc* ifd);
int if_inc_cur_txring (ifdesc* ifd);
int if_set_cur_txring_to_last (ifdesc* ifd);
int if_set_cur_txring (ifdesc* ifd, uint16_t idx);
int if_reset_cur_rxring (ifdesc* ifd);
int if_inc_cur_rxring (ifdesc* ifd);
int if_set_cur_rxring_to_last (ifdesc* ifd);
int if_set_cur_rxring (ifdesc* ifd, uint16_t idx);

/* Set and get the current tx or rx ring. It is not done in a circular fashion,
 * next and following may return NULL if the last one is reached. */
ifring* if_next_txring (ifdesc* ifd);
ifring* if_next_rxring (ifdesc* ifd);

/* Set the current buffer idx of a ring to head, +num or next.
 * Return the set id. */
uint32_t ifring_rewind (ifring* ring);
uint32_t ifring_wait_for_more (ifring* ring, uint32_t num);
uint32_t ifring_next (ifring* ring);

/* Set the head buffer idx of a ring to next, cur, tail or <idx>
 * Return the set id. */
uint32_t ifring_release_one (ifring* ring);
uint32_t ifring_release_done (ifring* ring);
uint32_t ifring_release_all (ifring* ring);
uint32_t ifring_set_head (ifring* ring, uint32_t idx);

/* Get the next buffer of a ring, incrementing cursor. Wraps around. */
char* ifring_next_buf (ifring* ring);

/* Get the next buffer length of a ring, incrementing cursor.  Wraps around.
 * Returns 0 when reaching the tail.*/
uint16_t ifring_next_len (ifring* ring);

/* Same as nm_inject. */
int if_inject (ifdesc* ifd, const void* buf, size_t len);

/* Same as nm_dispatch. */
int if_dispatch (ifdesc* ifd, int cnt, ifpkt_hn handler,
	unsigned char* arg);

#endif
