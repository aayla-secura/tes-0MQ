/*
 * This is an API for setting and getting the fields of ring structures in an
 * opaque way (clients should only deal with pointers to them and pass them to
 * the methods declared in the header files). This is to ensure that clients
 * including only fpgaif_reader.h cannot modify the data.
 *
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * –––––––––––––––––––––––––––––––– DEV NOTES –––––––––––––––––––––––––––––––––
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * For now this is just a wrapper around netmap. We can either never our struct
 * types and always cast the pointer to the corresponding netmap structure, or
 * define our structures to include a single member that is the corresponding
 * netmap structure. Go with the second (we can also access the members using
 * pointer cast in this case, but better stick to using the netmap member
 * directly).
 *
 * Netmap uses two user-driven constructs---a head and a cursor. The head tells
 * it which slots it can safely free, while the cursor tells it when to unblock
 * a poll call. When the head lags behind the tail, the cursor must never be
 * set to a slot index in the range head+1 ... tail because the poll would
 * block forever (the tail will reach the head before it reaches the cursor).
 * Hence we name 'done' packets in the range head ... cur-1 and 'pending' packets
 * in the range cur ... tail-1.
 *
 * We use 'next' when use the cursor and increment it, and use 'following' when
 * we use a given id return the corresponding object associated with the id
 * following it (not touching the cursor).
 *
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * –––––––––––––––––––––––––––––––––– TO DO –––––––––––––––––––––––––––––––––––
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * - More rigorous checks when setting head or cursor. In particular, ensure
 *   cursor never ends up between head and tail since netmap poll will block
 *   forever.
 * - Provide a way to build an ifreq object.
 * - A read-only dispatcher (takes a pointer to head and cursor).
 */

#include "net/fpgaif_manager.h"

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h> /* defines 'unlikely' macro */

struct _ifring
{
	struct netmap_ring n;
};

struct _ifdesc
{
	struct nm_desc n;
};

struct _ifreq
{
	struct nmreq n;
};

struct _ifhdr
{
	struct nm_pkthdr n;
};

/* ------------------------------------------------------------------------- */

static inline uint32_t
s_ring_following (ifring* ring, uint32_t idx)
{ /* same as nm_ring_next */
	return (unlikely(idx + 1 == ring->n.num_slots) ? 0 : idx + 1);
}
static inline char*
s_buf (ifring* ring, uint32_t idx)
{
	return NETMAP_BUF (&ring->n, ring->n.slot[ idx ].buf_idx);
}
static inline ifring*
s_txring (ifdesc* ifd, uint16_t idx)
{
	return (ifring*)NETMAP_TXRING (ifd->n.nifp, idx);
}
static inline ifring*
s_rxring (ifdesc* ifd, uint16_t idx)
{
	return (ifring*)NETMAP_RXRING (ifd->n.nifp, idx);
}

/* -------------------------------- MANAGER -------------------------------- */


/* Open or close an interface. */
ifdesc*
if_open (const char *name, const ifreq *req,
	uint64_t flags, const ifdesc *arg)
{
	return (ifdesc*) nm_open (name, &req->n, flags, &arg->n);
}
int
if_close (ifdesc* ifd)
{ /* to keep the signature of nm_close we don't take a double pointer, so
   * caller should nullify it */
	return nm_close (&ifd->n);
}

/* Get the file descriptor */
int
if_fd (ifdesc* ifd)
{
	return ifd->n.fd;
}

/* Set the current tx or rx ring to first, next, last or <idx>.
 * Returns 0 on succes, -1 on error. */
int
if_reset_cur_txring (ifdesc* ifd)
{
	ifd->n.cur_tx_ring = ifd->n.first_tx_ring;
	return 0;
}
int
if_inc_cur_txring (ifdesc* ifd)
{
	if (unlikely (ifd->n.cur_tx_ring > ifd->n.last_tx_ring))
		return -1;
	ifd->n.cur_tx_ring++;
	return 0;
}
int
if_set_cur_txring_to_last (ifdesc* ifd)
{
	ifd->n.cur_tx_ring = ifd->n.last_tx_ring;
	return 0;
}
int
if_set_cur_txring (ifdesc* ifd, uint16_t idx)
{
	if (unlikely (idx > ifd->n.last_tx_ring))
		return -1;
	ifd->n.cur_tx_ring = idx;
	return 0;
}
int
if_reset_cur_rxring (ifdesc* ifd)
{
	ifd->n.cur_rx_ring = ifd->n.first_rx_ring;
	return 0;
}
int
if_inc_cur_rxring (ifdesc* ifd)
{
	if (unlikely (ifd->n.cur_rx_ring > ifd->n.last_rx_ring))
		return -1;
	ifd->n.cur_rx_ring++;
	return 0;
}
int
if_set_cur_rxring_to_last (ifdesc* ifd)
{
	ifd->n.cur_rx_ring = ifd->n.last_rx_ring;
	return 0;
}
int
if_set_cur_rxring (ifdesc* ifd, uint16_t idx)
{
	if (unlikely (idx > ifd->n.last_rx_ring))
		return -1;
	ifd->n.cur_rx_ring = idx;
	return 0;
}

/* Set and get the current tx or rx ring. It is not done in a circular fashion,
 * next and following may return NULL if the last one is reached. */
ifring*
if_next_txring (ifdesc* ifd)
{
	if (unlikely (ifd->n.cur_tx_ring == ifd->n.last_tx_ring))
		return NULL;
	return (ifring*)NETMAP_TXRING (ifd->n.nifp, ++ifd->n.cur_tx_ring);
}
ifring*
if_next_rxring (ifdesc* ifd)
{
	if (unlikely (ifd->n.cur_rx_ring == ifd->n.last_rx_ring))
		return NULL;
	return (ifring*)NETMAP_RXRING (ifd->n.nifp, ++ifd->n.cur_rx_ring);
}

/* Set the current buffer idx of a ring to head, next or tail+num.
 * Return the set id. */
uint32_t
ifring_rewind (ifring* ring)
{
	ring->n.cur = ring->n.head;
	return ring->n.cur;
}
uint32_t
ifring_next (ifring* ring)
{
	ring->n.cur = s_ring_following (ring, ring->n.cur);
	return ring->n.cur;
}
uint32_t
ifring_wait_for_more (ifring* ring, uint32_t num)
{
	ring->n.cur = ring->n.tail + num;
#ifdef MIN_CHECKS
	if (ring->n.cur >= ring->n.num_slots)
#else
	while (ring->n.cur >= ring->n.num_slots)
#endif
		ring->n.cur -= ring->n.num_slots;
	return ring->n.cur;
}

/* Set the head buffer idx of a ring to next or cur.
 * Return the set id. */
uint32_t
ifring_release_one (ifring* ring)
{
#ifndef MIN_CHECKS
	if (unlikely (ring->n.head == ring->n.tail))
		return ring->n.head;
#endif
	ring->n.head = s_ring_following (ring, ring->n.head);
	return ring->n.head;
}
uint32_t
ifring_release_done (ifring* ring)
{
	/* TO DO: set to tail if cur is after tail? */
	ring->n.head = ring->n.cur;
	return ring->n.head;
}

/* Set both the head and cursor to tail.
 * Return the set id. */
uint32_t
ifring_release_all (ifring* ring)
{
	ring->n.head = ring->n.cur = ring->n.tail;
	return ring->n.head;
}

/* Set the cursor and optionally head to <idx>. */
void
ifring_goto (ifring* ring, uint32_t idx, int sync_h)
{
	/* TO DO: check if its after tail? */
	ring->n.cur = idx;
	if (sync_h)
		ring->n.head = idx;
}

/* Set the head to <idx>. */
void
ifring_release_to (ifring* ring, uint32_t idx)
{
	/* TO DO: check if its after tail? */
	ring->n.head = idx;
}

/* Get the next buffer of a ring, incrementing cursor. Wraps around.
 * Returns NULL when reaching the tail. */
char*
ifring_next_buf (ifring* ring)
{
	ring->n.cur = s_ring_following (ring, ring->n.cur);
#ifndef MIN_CHECKS
	if (unlikely (ring->n.cur == ring->n.tail))
		return NULL;
#endif
	return s_buf (ring, ring->n.cur);
}

/* Get the next buffer length of a ring, incrementing cursor.  Wraps around.
 * Returns 0 when reaching the tail.*/
uint16_t
ifring_next_len (ifring* ring)
{
	ring->n.cur = s_ring_following (ring, ring->n.cur);
#ifndef MIN_CHECKS
	if (unlikely (ring->n.cur == ring->n.tail))
		return 0;
#endif
	return ring->n.slot[ ring->n.cur ].len;
}

/* Same as nm_inject. */
int
if_inject (ifdesc* ifd, const void* buf, size_t len)
{
	return nm_inject (&ifd->n, buf, len);
}

/* Same as nm_dispatch. */
int
if_dispatch (ifdesc* ifd, int cnt, ifpkt_hn handler,
	unsigned char* arg)
{
	/* The cast to nm_cb_t suppresses the GCC warning, due to ifpkt_hn
	 * accepting an ifhdr* rather than (the equivalent) struct nm_pkthdr* */
	return nm_dispatch (&ifd->n, cnt, (nm_cb_t)handler, arg);
}

/* -------------------------------- READER --------------------------------- */


/* Get the number of rings. */
uint16_t
if_txrings (ifdesc* ifd)
{
	return (ifd->n.last_tx_ring - ifd->n.first_tx_ring + 1);
}
uint16_t
if_rxrings (ifdesc* ifd)
{
	return (ifd->n.last_rx_ring - ifd->n.first_rx_ring + 1);
}

/* Get a ring. It is not done in a circular fashion, next and following return
 * NULL if the last one is reached. */
ifring*
if_first_txring (ifdesc* ifd)
{
	return s_txring (ifd, ifd->n.first_tx_ring);
}
ifring*
if_cur_txring (ifdesc* ifd)
{
	return s_txring (ifd, ifd->n.cur_tx_ring);
}
ifring*
if_following_txring (ifdesc* ifd, uint16_t idx)
{
	if (unlikely (ifd->n.cur_tx_ring == ifd->n.last_tx_ring))
		return NULL;
	return s_txring (ifd, ifd->n.cur_tx_ring + 1);
}
ifring*
if_last_txring (ifdesc* ifd)
{
	return s_txring (ifd, ifd->n.last_tx_ring);
}
ifring*
if_first_rxring (ifdesc* ifd)
{
	return s_rxring (ifd, ifd->n.first_rx_ring);
}
ifring*
if_cur_rxring (ifdesc* ifd)
{
	return s_rxring (ifd, ifd->n.cur_rx_ring);
}
ifring*
if_following_rxring (ifdesc* ifd, uint16_t idx)
{
	if (unlikely (ifd->n.cur_rx_ring == ifd->n.last_rx_ring))
		return NULL;
	return s_rxring (ifd, ifd->n.cur_rx_ring + 1);
}
ifring*
if_last_rxring (ifdesc* ifd)
{
	return s_rxring (ifd, ifd->n.last_rx_ring);
}

/* Get the number of buffers. */
uint32_t
ifring_bufs (ifring* ring)
{
	return ring->n.num_slots;
}

/* Get the physical size of the buffers in the ring. */
uint32_t
ifring_buf_size (ifring* ring)
{
	return ring->n.nr_buf_size;
}

/* Get the head, current, following <id> or tail buffer id of a ring. Wraps
 * around. */
uint32_t
ifring_head (ifring* ring)
{
	return ring->n.head;
}
uint32_t
ifring_cur (ifring* ring)
{
	return ring->n.cur;
}
uint32_t
ifring_following (ifring* ring, uint32_t idx)
{
	return s_ring_following (ring, idx);
}
uint32_t
ifring_tail (ifring* ring)
{
	return ring->n.tail;
}

/* Compare slots mod num_slots taking into accout the ring's head.
 * Returns -1 or 1 if ida is closer or farther from the head than idb.
 * Returns 0 if they are equal. */
int
ifring_compare_ids (ifring* ring, uint32_t ida, uint32_t idb)
{
	if (unlikely (ida == idb))
		return 0;

	/* If both are in the same region of the ring (i.e. numerically both
	 * are < or both are > head, then the numerically smaller is first,
	 * otherwise, the numerically larger is first. */
	if ( (ring->n.head <= ida && ring->n.head <= idb) ||
		(ring->n.head > ida && ring->n.head > idb) )
		return (ida < idb) ? -1 : 1;

	return (ida < idb) ? 1 : -1;
}
/* Compare slots mod num_slots taking into accout the ring's head.
 * Returns the buf idx that is closer (smaller) or farther (larger) to the
 * ring's head in a forward direction. */
uint32_t
ifring_earlier_id (ifring* ring, uint32_t ida, uint32_t idb)
{
	if (unlikely (ida == idb))
		return ida;

	if ( (ring->n.head <= ida && ring->n.head <= idb) ||
		(ring->n.head > ida && ring->n.head > idb) )
		return (ida < idb) ? ida : idb;

	return (ida < idb) ? idb : ida;
}
uint32_t
ifring_later_id (ifring* ring, uint32_t ida, uint32_t idb)
{
	if (unlikely (ida == idb))
		return ida;

	if ( (ring->n.head <= ida && ring->n.head <= idb) ||
		(ring->n.head > ida && ring->n.head > idb) )
		return (ida < idb) ? idb : ida;

	return (ida < idb) ? ida : idb;
}

/* Get the head, current, <id>, following <id> or tail buffer of a ring. Wraps
 * around. Returns NULL for buffers in the range tail ... head-1. */
char*
ifring_head_buf (ifring* ring)
{
	return s_buf (ring, ring->n.head);
}
char*
ifring_cur_buf (ifring* ring)
{
#ifdef MIN_CHECKS
	return s_buf (ring, ring->n.cur);
#else
	if (ifring_compare_ids (ring, ring->n.cur, ring->n.tail) == -1)
		return s_buf (ring, ring->n.cur);
	return NULL;
#endif
}
char*
ifring_buf (ifring* ring, uint32_t idx)
{
#ifdef MIN_CHECKS
	return s_buf (ring, idx);
#else
	if (ifring_compare_ids (ring, idx, ring->n.tail) == -1)
		return s_buf (ring, idx);
	return NULL;
#endif
}
char*
ifring_following_buf (ifring* ring, uint32_t idx)
{
#ifdef MIN_CHECKS
	return s_buf (ring, s_ring_following (ring, idx));
#else
	idx = s_ring_following (ring, idx);
	if (ifring_compare_ids (ring, idx, ring->n.tail) == -1)
		return s_buf (ring, idx);
	return NULL;
#endif
}
char*
ifring_last_buf (ifring* ring)
{
	if (unlikely (ring->n.tail == 0))
		return s_buf (ring, ring->n.num_slots - 1);
	return s_buf (ring, ring->n.tail - 1);
}

/* Get the head, current, <id>, following <id> or tail buffer length of a ring.
 * Wraps around.
 * Returns 0 when reaching the tail when incrementing by 1 (does not check
 * a given idx though).*/
uint16_t
ifring_head_len (ifring* ring)
{
	return ring->n.slot[ ring->n.head ].len;
}
uint16_t
ifring_cur_len (ifring* ring)
{
#ifdef MIN_CHECKS
	return ring->n.slot[ ring->n.cur ].len;
#else
	if (ifring_compare_ids (ring, ring->n.cur, ring->n.tail) == -1)
		return ring->n.slot[ ring->n.cur ].len;
	return 0;
#endif
}
uint16_t
ifring_len (ifring* ring, uint32_t idx)
{
#ifdef MIN_CHECKS
	return ring->n.slot[ idx ].len;
#else
	if (ifring_compare_ids (ring, idx, ring->n.tail) == -1)
		return ring->n.slot[ idx ].len;
	return 0;
#endif
}
uint16_t
ifring_following_len (ifring* ring, uint32_t idx)
{
#ifdef MIN_CHECKS
	return ring->n.slot[ idx ].len;
#else
	idx = s_ring_following (ring, idx);
	if (ifring_compare_ids (ring, idx, ring->n.tail) == -1)
		return ring->n.slot[ idx ].len;
	return 0;
#endif
}
uint16_t
ifring_last_len (ifring* ring)
{
	if (unlikely (ring->n.tail == 0))
		return ring->n.slot[ ring->n.num_slots - 1 ].len;
	return ring->n.slot[ ring->n.tail - 1 ].len;
}

/* Get the number of slots between cur and tail.
 * For rx rings, this is the number of uninspected received slots.
 * For tx rings, this is the number of free slots. */
uint32_t
ifring_pending (ifring* ring)
{
        int ret = ring->n.tail - ring->n.cur;
        if (ret < 0)
                ret += ring->n.num_slots;
        return (uint32_t) ret;
}

/* Get the number of slots between head and cur.
 * For rx rings, this is the number of inspected received slots.
 * For tx rings, this is the number of received slots. */
uint32_t
ifring_done (ifring* ring)
{
        int ret = ring->n.cur - ring->n.head;
        if (ret < 0)
                ret += ring->n.num_slots;
        return (uint32_t) ret;
}

/* Get the number of slots between head and tail.
 * For rx rings, this is the total number of received slots.
 * For tx rings, this is the number of received + free slots. */
uint32_t
ifring_total (ifring* ring)
{
        int ret = ring->n.tail - ring->n.head;
        if (ret < 0)
                ret += ring->n.num_slots;
        return (uint32_t) ret;
}
