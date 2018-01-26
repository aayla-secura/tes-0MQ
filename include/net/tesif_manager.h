/*
 * An opaque class for managing the ring structure. At the moment it's just
 * a wrapper around netmap.
 */

#ifndef __NET_TESIF_MANAGER_H_INCLUDED__
#define __NET_TESIF_MANAGER_H_INCLUDED__

#include "net/tesif_reader.h"
#include <sys/types.h>

/*
 * Open or close an interface.
 */
tes_ifdesc* tes_if_open (const char *name, const tes_ifreq *req,
	uint64_t flags, const tes_ifdesc *arg);
int tes_if_close (tes_ifdesc* ifd);

/*
 * Get the file descriptor
 */
int tes_if_fd (tes_ifdesc* ifd);

/*
 * Get the interface name
 */
char* tes_if_name (tes_ifdesc* ifd);

/*
 * Set and get the first, previous, next, <idx> or last tx or rx ring.
 * It is not done in a circular fashion.
 * Returns NULL for rings beyond the last one.
 */
tes_ifring* tes_if_rewind_txring (tes_ifdesc* ifd);
tes_ifring* tes_if_previous_txring (tes_ifdesc* ifd);
tes_ifring* tes_if_next_txring (tes_ifdesc* ifd);
tes_ifring* tes_if_goto_txring (tes_ifdesc* ifd, uint16_t idx);
tes_ifring* tes_if_goto_last_txring (tes_ifdesc* ifd);
tes_ifring* tes_if_rewind_rxring (tes_ifdesc* ifd);
tes_ifring* tes_if_previous_rxring (tes_ifdesc* ifd);
tes_ifring* tes_if_next_rxring (tes_ifdesc* ifd);
tes_ifring* tes_if_goto_rxring (tes_ifdesc* ifd, uint16_t idx);
tes_ifring* tes_if_goto_last_rxring (tes_ifdesc* ifd);

/*
 * Set and get the current buffer of a ring to head, previous, next, <idx> or
 * tail-1.
 * previous and next return NULL when reaching the head-1 or tail.
 */
char* tes_ifring_rewind_buf (tes_ifring* ring);
char* tes_ifring_previous_buf (tes_ifring* ring);
char* tes_ifring_next_buf (tes_ifring* ring);
char* tes_ifring_goto_buf (tes_ifring* ring, uint32_t idx);
char* tes_ifring_goto_last_buf (tes_ifring* ring);

/*
 * Set and get the head buffer of a ring to next, <idx> or cur.
 */
char* tes_ifring_release_one_buf (tes_ifring* ring);
char* tes_ifring_release_to_buf (tes_ifring* ring, uint32_t idx);
char* tes_ifring_release_done_buf (tes_ifring* ring);

/*
 * Set the current buffer of a ring to tail+num.
 */
void tes_ifring_wait_for_more (tes_ifring* ring, uint32_t num);

/*
 * Set both the head and cursor to tail.
 */
void tes_ifring_release_all (tes_ifring* ring);

/*
 * Same as nm_inject and nm_dispatch.
 */
int tes_if_inject (tes_ifdesc* ifd, const void* buf, size_t len);
int tes_if_dispatch (tes_ifdesc* ifd, int cnt, tes_ifpkt_hn handler,
	unsigned char* arg);

#endif
