/*
 * An opaque class for reading the ring structure. At the moment it's just
 * a wrapper around netmap.
 */

#ifndef __NET_FPGAIF_READER_H_INCLUDED__
#define __NET_FPGAIF_READER_H_INCLUDED__

#include <stdint.h>

typedef struct _ifring ifring;
typedef struct _ifdesc ifdesc;
typedef struct _ifreq ifreq;
typedef struct _ifhdr ifhdr;

typedef void (ifpkt_hn)(unsigned char *, const ifhdr*,
	const unsigned char *buf);

/* Get the number of tx or rx rings. */
uint16_t if_txrings (ifdesc* ifd);
uint16_t if_rxrings (ifdesc* ifd);

/* Get the first, current, <idx>, preceding <idx>, following <idx> or last tx
 * or rx ring. It is not done in a circular fashion.
 * Returns NULL for rings beyond the last one. */
ifring* if_first_txring (ifdesc* ifd);
ifring* if_cur_txring (ifdesc* ifd);
ifring* if_txring (ifdesc* ifd, uint16_t idx);
ifring* if_preceding_txring (ifdesc* ifd, uint16_t idx);
ifring* if_following_txring (ifdesc* ifd, uint16_t idx);
ifring* if_last_txring (ifdesc* ifd);
ifring* if_first_rxring (ifdesc* ifd);
ifring* if_cur_rxring (ifdesc* ifd);
ifring* if_rxring (ifdesc* ifd, uint16_t idx);
ifring* if_preceding_rxring (ifdesc* ifd, uint16_t idx);
ifring* if_following_rxring (ifdesc* ifd, uint16_t idx);
ifring* if_last_rxring (ifdesc* ifd);

/* Get the number of buffers in the ring. */
uint32_t ifring_bufs (ifring* ring);

/* Get the physical size of the buffers in the ring. */
uint32_t ifring_buf_size (ifring* ring);

/* Compare slots mod num_slots taking into accout the ring's head.
 * Returns -1 or 1 if ida is closer or farther from the head than idb.
 * Returns 0 if they are equal. */
int ifring_compare_ids (ifring* ring, uint32_t ida, uint32_t idb);

/* Compare slots mod num_slots taking into accout the ring's head.
 * Returns the buf id that is closer (smaller) or farther (larger) to the
 * ring's head in a forward direction. */
uint32_t ifring_earlier_id (ifring* ring, uint32_t ida, uint32_t idb);
uint32_t ifring_later_id (ifring* ring, uint32_t ida, uint32_t idb);

/* Get the head, current, preceding <idx>, following <idx> or tail buffer id of
 * a ring. Wraps around. */
uint32_t ifring_head (ifring* ring);
uint32_t ifring_cur (ifring* ring);
uint32_t ifring_preceding (ifring* ring, uint32_t idx);
uint32_t ifring_following (ifring* ring, uint32_t idx);
uint32_t ifring_tail (ifring* ring);

/* Get the head, current, <idx>, preceding <idx>, following <idx> or tail
 * buffer of a ring. Wraps around.
 * preceding and following return NULL when reaching the head-1 or tail. */
char* ifring_head_buf (ifring* ring);
char* ifring_cur_buf (ifring* ring);
char* ifring_buf (ifring* ring, uint32_t idx);
char* ifring_preceding_buf (ifring* ring, uint32_t idx);
char* ifring_following_buf (ifring* ring, uint32_t idx);
char* ifring_last_buf (ifring* ring);

/* Get the length of the current or <idx> buffer. */
uint16_t ifring_cur_len (ifring* ring);
uint16_t ifring_len (ifring* ring, uint32_t idx);

/* Get the number of slots between cur and tail.
 * For rx rings, this is the number of uninspected received slots.
 * For tx rings, this is the number of free slots. */
uint32_t ifring_pending (ifring* ring);

/* Get the number of slots between head and cur.
 * For rx rings, this is the number of inspected received slots.
 * For tx rings, this is the number of received slots. */
uint32_t ifring_done (ifring* ring);

/* Get the number of slots between head and tail.
 * For rx rings, this is the total number of received slots.
 * For tx rings, this is the number of received + free slots. */
uint32_t ifring_total (ifring* ring);

#endif
