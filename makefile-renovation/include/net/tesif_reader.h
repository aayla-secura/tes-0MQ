/*
 * An opaque class for reading the ring structure. At the moment it's just
 * a wrapper around netmap.
 */

#ifndef __NET_TESIF_READER_H_INCLUDED__
#define __NET_TESIF_READER_H_INCLUDED__

#include <stdint.h>

typedef struct tes_ifring tes_ifring;
typedef struct tes_ifdesc tes_ifdesc;
typedef struct tes_ifreq  tes_ifreq;
typedef struct tes_ifhdr  tes_ifhdr;

typedef void (tes_ifpkt_hn)(unsigned char *, const tes_ifhdr*,
	const unsigned char *buf);

/* Get the number of tx or rx rings. */
uint16_t tes_if_txrings (tes_ifdesc* ifd);
uint16_t tes_if_rxrings (tes_ifdesc* ifd);

/* Get the first, current, <idx>, preceding <idx>, following <idx> or last tx
 * or rx ring. It is not done in a circular fashion.
 * Returns NULL for rings beyond the last one. */
tes_ifring* tes_if_first_txring (tes_ifdesc* ifd);
tes_ifring* tes_if_cur_txring (tes_ifdesc* ifd);
tes_ifring* tes_if_txring (tes_ifdesc* ifd, uint16_t idx);
tes_ifring* tes_if_preceding_txring (tes_ifdesc* ifd, uint16_t idx);
tes_ifring* tes_if_following_txring (tes_ifdesc* ifd, uint16_t idx);
tes_ifring* tes_if_last_txring (tes_ifdesc* ifd);
tes_ifring* tes_if_first_rxring (tes_ifdesc* ifd);
tes_ifring* tes_if_cur_rxring (tes_ifdesc* ifd);
tes_ifring* tes_if_rxring (tes_ifdesc* ifd, uint16_t idx);
tes_ifring* tes_if_preceding_rxring (tes_ifdesc* ifd, uint16_t idx);
tes_ifring* tes_if_following_rxring (tes_ifdesc* ifd, uint16_t idx);
tes_ifring* tes_if_last_rxring (tes_ifdesc* ifd);

/* Get the index of the first, current or last tx or rx ring. */
uint16_t tes_if_cur_txring_id (tes_ifdesc* ifd);
uint16_t tes_if_last_txring_id (tes_ifdesc* ifd);
uint16_t tes_if_cur_rxring_id (tes_ifdesc* ifd);
uint16_t tes_if_last_rxring_id (tes_ifdesc* ifd);

/* Get the number of buffers in the ring. */
uint32_t tes_ifring_bufs (tes_ifring* ring);

/* Get the physical size of the buffers in the ring. */
uint32_t tes_ifring_buf_size (tes_ifring* ring);

/* Compare slots mod num_slots taking into accout the ring's head.
 * Returns -1 or 1 if ida is closer or farther from the head than idb.
 * Returns 0 if they are equal. */
int tes_ifring_compare_ids (tes_ifring* ring, uint32_t ida, uint32_t idb);

/* Compare slots mod num_slots taking into accout the ring's head.
 * Returns the buf id that is closer (smaller) or farther (larger) to the
 * ring's head in a forward direction. */
uint32_t tes_ifring_earlier_id (tes_ifring* ring, uint32_t ida, uint32_t idb);
uint32_t tes_ifring_later_id (tes_ifring* ring, uint32_t ida, uint32_t idb);

/* Get the head, current, preceding <idx>, following <idx> or tail buffer id of
 * a ring. Wraps around. */
uint32_t tes_ifring_head (tes_ifring* ring);
uint32_t tes_ifring_cur (tes_ifring* ring);
uint32_t tes_ifring_preceding (tes_ifring* ring, uint32_t idx);
uint32_t tes_ifring_following (tes_ifring* ring, uint32_t idx);
uint32_t tes_ifring_tail (tes_ifring* ring);

/* Get the current tx or rx ring's current buffer. */
char* tes_if_cur_txbuf (tes_ifdesc* ifd);
char* tes_if_cur_rxbuf (tes_ifdesc* ifd);

/* Get the head, current, <idx>, preceding <idx>, following <idx> or tail
 * buffer of a ring. Wraps around.
 * preceding and following return NULL when reaching the head-1 or tail. */
char* tes_ifring_head_buf (tes_ifring* ring);
char* tes_ifring_cur_buf (tes_ifring* ring);
char* tes_ifring_buf (tes_ifring* ring, uint32_t idx);
char* tes_ifring_preceding_buf (tes_ifring* ring, uint32_t idx);
char* tes_ifring_following_buf (tes_ifring* ring, uint32_t idx);
char* tes_ifring_last_buf (tes_ifring* ring);

/* Get the length of the current or <idx> buffer. */
uint16_t tes_ifring_cur_len (tes_ifring* ring);
uint16_t tes_ifring_len (tes_ifring* ring, uint32_t idx);

/* Get the number of slots between cur and tail.
 * For rx rings, this is the number of uninspected received slots.
 * For tx rings, this is the number of free slots. */
uint32_t tes_ifring_pending (tes_ifring* ring);

/* Get the number of slots between head and cur.
 * For rx rings, this is the number of inspected received slots.
 * For tx rings, this is the number of received slots. */
uint32_t tes_ifring_done (tes_ifring* ring);

/* Get the number of slots between head and tail.
 * For rx rings, this is the total number of received slots.
 * For tx rings, this is the number of received + free slots. */
uint32_t tes_ifring_total (tes_ifring* ring);

#endif
