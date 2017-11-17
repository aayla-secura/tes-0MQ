/*
 * This is an API for setting and getting the fields of ring structures in an
 * opaque way (clients should only deal with pointers to them and pass them to
 * the methods declared in the header files). This is to ensure that clients
 * including only tesif_reader.h cannot modify the data.
 *
 * ----------------------------------------------------------------------------
 * -------------------------------- DEV NOTES ---------------------------------
 * ----------------------------------------------------------------------------
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
 * ----------------------------------------------------------------------------
 * ---------------------------------- TO DO -----------------------------------
 * ----------------------------------------------------------------------------
 * - More rigorous checks when setting head or cursor. In particular, ensure
 *   cursor never ends up between head and tail since netmap poll will block
 *   forever.
 * - Provide a way to build an tes_ifreq object.
 * - Subtract netmap's first ring ID from all ring IDs returned, so user is
 *   ensured IDs start at 0.
 */

#include "net/tesif_manager.h"

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h> /* defines 'unlikely' macro */

struct tes_ifring
{
	struct netmap_ring n;
};

struct tes_ifdesc
{
	struct nm_desc n;
};

struct tes_ifreq
{
	struct nmreq n;
};

struct tes_ifhdr
{
	struct nm_pkthdr n;
};

/* ------------------------------------------------------------------------- */

static inline uint32_t
s_ring_preceding (tes_ifring* ring, uint32_t idx)
{
	if (unlikely (idx == 0))
		return ring->n.num_slots - 1;
	return idx - 1;
}
static inline uint32_t
s_ring_following (tes_ifring* ring, uint32_t idx)
{
	if (unlikely (idx + 1 == ring->n.num_slots))
		return 0;
	return idx + 1;
}
static inline char*
s_buf (tes_ifring* ring, uint32_t idx)
{
	return NETMAP_BUF (&ring->n, ring->n.slot[ idx ].buf_idx);
}
static inline tes_ifring*
s_txring (tes_ifdesc* ifd, uint16_t idx)
{
	return (tes_ifring*)NETMAP_TXRING (ifd->n.nifp, idx);
}
static inline tes_ifring*
s_rxring (tes_ifdesc* ifd, uint16_t idx)
{
	return (tes_ifring*)NETMAP_RXRING (ifd->n.nifp, idx);
}

/* -------------------------------- MANAGER -------------------------------- */


/* Open or close an interface. */
tes_ifdesc*
tes_if_open (const char *name, const tes_ifreq *req,
	uint64_t flags, const tes_ifdesc *arg)
{
	return (tes_ifdesc*)nm_open (name, &req->n, flags, &arg->n);
}
int
tes_if_close (tes_ifdesc* ifd)
{ /* to keep the signature of nm_close we don't take a double pointer, so
   * caller should nullify it */
	return nm_close (&ifd->n);
}

/* Get the file descriptor */
int
tes_if_fd (tes_ifdesc* ifd)
{
	return ifd->n.fd;
}

/* Get the interface name */
char*
tes_if_name (tes_ifdesc* ifd)
{
	return ifd->n.nifp->ni_name;
}

/* Set and get the first, previous, next, <idx> or last tx or rx ring.
 * Returns NULL for rings beyond the last one. */
tes_ifring*
tes_if_rewind_txring (tes_ifdesc* ifd)
{
	ifd->n.cur_tx_ring = ifd->n.first_tx_ring;
	return s_txring (ifd, ifd->n.cur_tx_ring);
}
tes_ifring*
tes_if_previous_txring (tes_ifdesc* ifd)
{
	if (unlikely (ifd->n.cur_tx_ring == ifd->n.first_tx_ring))
		return NULL;
	return s_txring (ifd, --ifd->n.cur_tx_ring);
}
tes_ifring*
tes_if_next_txring (tes_ifdesc* ifd)
{
	if (unlikely (ifd->n.cur_tx_ring == ifd->n.last_tx_ring))
		return NULL;
	return s_txring (ifd, ++ifd->n.cur_tx_ring);
}
tes_ifring*
tes_if_goto_txring (tes_ifdesc* ifd, uint16_t idx)
{
	idx += ifd->n.first_tx_ring;
	if (unlikely (idx > ifd->n.last_tx_ring))
		return NULL;
	ifd->n.cur_tx_ring = idx;
	return s_txring (ifd, ifd->n.cur_tx_ring);
}
tes_ifring*
tes_if_goto_last_txring (tes_ifdesc* ifd)
{
	ifd->n.cur_tx_ring = ifd->n.last_tx_ring;
	return s_txring (ifd, ifd->n.cur_tx_ring);
}
tes_ifring*
tes_if_rewind_rxring (tes_ifdesc* ifd)
{
	ifd->n.cur_rx_ring = ifd->n.first_rx_ring;
	return s_rxring (ifd, ifd->n.cur_rx_ring);
}
tes_ifring*
tes_if_previous_rxring (tes_ifdesc* ifd)
{
	if (unlikely (ifd->n.cur_rx_ring == ifd->n.first_rx_ring))
		return NULL;
	return s_rxring (ifd, --ifd->n.cur_rx_ring);
}
tes_ifring*
tes_if_next_rxring (tes_ifdesc* ifd)
{
	if (unlikely (ifd->n.cur_rx_ring == ifd->n.last_rx_ring))
		return NULL;
	return s_rxring (ifd, ++ifd->n.cur_rx_ring);
}
tes_ifring*
tes_if_goto_rxring (tes_ifdesc* ifd, uint16_t idx)
{
	idx += ifd->n.first_rx_ring;
	if (unlikely (idx > ifd->n.last_rx_ring))
		return NULL;
	ifd->n.cur_rx_ring = idx;
	return s_rxring (ifd, ifd->n.cur_rx_ring);
}
tes_ifring*
tes_if_goto_last_rxring (tes_ifdesc* ifd)
{
	ifd->n.cur_rx_ring = ifd->n.last_rx_ring;
	return s_rxring (ifd, ifd->n.cur_rx_ring);
}

/* Set and get the current buffer of a ring to head, previous, next, <idx> or
 * tail-1.
 * previous and next return NULL when reaching the head-1 or tail. */
char*
tes_ifring_rewind_buf (tes_ifring* ring)
{
	ring->n.cur = ring->n.head;
	return s_buf (ring, ring->n.cur);
}
char*
tes_ifring_previous_buf (tes_ifring* ring)
{
	if (unlikely (ring->n.cur == ring->n.head))
		return NULL;
	ring->n.cur = s_ring_preceding (ring, ring->n.cur);
	return s_buf (ring, ring->n.cur);
}
char*
tes_ifring_next_buf (tes_ifring* ring)
{
	ring->n.cur = s_ring_following (ring, ring->n.cur);
	if (unlikely (ring->n.cur == ring->n.tail))
		return NULL;
	return s_buf (ring, ring->n.cur);
}
char*
tes_ifring_goto_buf (tes_ifring* ring, uint32_t idx)
{
	ring->n.cur = idx;
	return s_buf (ring, ring->n.cur);
}
char*
tes_ifring_goto_last_buf (tes_ifring* ring)
{
	ring->n.cur = s_ring_preceding (ring, ring->n.tail);
	return s_buf (ring, ring->n.cur);
}

/* Set and get the head buffer of a ring to next, <idx> or cur. */
char*
tes_ifring_release_one_buf (tes_ifring* ring)
{
	ring->n.head = s_ring_following (ring, ring->n.head);
	return s_buf (ring, ring->n.head);
}
char*
tes_ifring_release_to_buf (tes_ifring* ring, uint32_t idx)
{
	ring->n.head = idx;
	return s_buf (ring, ring->n.head);
}
char*
tes_ifring_release_done_buf (tes_ifring* ring)
{
	ring->n.head = ring->n.cur;
	return s_buf (ring, ring->n.head);
}

/* Set the current buffer of a ring to tail+num. */
void
tes_ifring_wait_for_more (tes_ifring* ring, uint32_t num)
{
	ring->n.cur = ring->n.tail + num;
	while (ring->n.cur >= ring->n.num_slots)
		ring->n.cur -= ring->n.num_slots;
}

/* Set both the head and current buffer to tail. */
void
tes_ifring_release_all (tes_ifring* ring)
{
	ring->n.head = ring->n.cur = ring->n.tail;
}

/* Same as nm_inject. */
int
tes_if_inject (tes_ifdesc* ifd, const void* buf, size_t len)
{
	return nm_inject (&ifd->n, buf, len);
}

/* Same as nm_dispatch. */
int
tes_if_dispatch (tes_ifdesc* ifd, int cnt, tes_ifpkt_hn handler,
	unsigned char* arg)
{
	/* The cast to nm_cb_t suppresses the GCC warning, due to tes_ifpkt_hn
	 * accepting an tes_ifhdr* rather than (the equivalent) struct nm_pkthdr* */
	return nm_dispatch (&ifd->n, cnt, (nm_cb_t)handler, arg);
}

/* -------------------------------- READER --------------------------------- */

/* Get the number of tx or rx rings. */
uint16_t
tes_if_txrings (tes_ifdesc* ifd)
{
	return (ifd->n.last_tx_ring - ifd->n.first_tx_ring + 1);
}
uint16_t
tes_if_rxrings (tes_ifdesc* ifd)
{
	return (ifd->n.last_rx_ring - ifd->n.first_rx_ring + 1);
}

/* Get the current tx or rx ring id. */
/* TO DO */

/* Get the first, current, <idx>, preceding <idx>, following <idx> or last tx
 * or rx ring. It is not done in a circular fashion.
 * Returns NULL for rings before first or beyond the last one. */
tes_ifring*
tes_if_first_txring (tes_ifdesc* ifd)
{
	return s_txring (ifd, ifd->n.first_tx_ring);
}
tes_ifring*
tes_if_cur_txring (tes_ifdesc* ifd)
{
	return s_txring (ifd, ifd->n.cur_tx_ring);
}
tes_ifring*
tes_if_txring (tes_ifdesc* ifd, uint16_t idx)
{
	idx += ifd->n.first_tx_ring;
	if (unlikely (idx < ifd->n.first_tx_ring
		|| idx > ifd->n.last_tx_ring))
		return NULL;
	return s_txring (ifd, idx);
}
tes_ifring*
tes_if_preceding_txring (tes_ifdesc* ifd, uint16_t idx)
{
	idx += ifd->n.first_tx_ring;
	if (unlikely (idx <= ifd->n.first_tx_ring ||
		idx - 1 > ifd->n.last_tx_ring))
		return NULL;
	return s_txring (ifd, idx - 1);
}
tes_ifring*
tes_if_following_txring (tes_ifdesc* ifd, uint16_t idx)
{
	idx += ifd->n.first_tx_ring;
	if (unlikely (idx + 1 < ifd->n.first_tx_ring ||
		idx >= ifd->n.last_tx_ring))
		return NULL;
	return s_txring (ifd, idx + 1);
}
tes_ifring*
tes_if_last_txring (tes_ifdesc* ifd)
{
	return s_txring (ifd, ifd->n.last_tx_ring);
}
tes_ifring*
tes_if_first_rxring (tes_ifdesc* ifd)
{
	return s_rxring (ifd, ifd->n.first_rx_ring);
}
tes_ifring*
tes_if_cur_rxring (tes_ifdesc* ifd)
{
	return s_rxring (ifd, ifd->n.cur_rx_ring);
}
tes_ifring*
tes_if_rxring (tes_ifdesc* ifd, uint16_t idx)
{
	idx += ifd->n.first_rx_ring;
	if (unlikely (idx < ifd->n.first_rx_ring
		|| idx > ifd->n.last_rx_ring))
		return NULL;
	return s_rxring (ifd, idx);
}
tes_ifring*
tes_if_preceding_rxring (tes_ifdesc* ifd, uint16_t idx)
{
	idx += ifd->n.first_rx_ring;
	if (unlikely (idx <= ifd->n.first_rx_ring ||
		idx - 1 > ifd->n.last_rx_ring))
		return NULL;
	return s_rxring (ifd, idx - 1);
}
tes_ifring*
tes_if_following_rxring (tes_ifdesc* ifd, uint16_t idx)
{
	idx += ifd->n.first_rx_ring;
	if (unlikely (idx + 1 < ifd->n.first_rx_ring ||
		idx >= ifd->n.last_rx_ring))
		return NULL;
	return s_rxring (ifd, idx + 1);
}
tes_ifring*
tes_if_last_rxring (tes_ifdesc* ifd)
{
	return s_rxring (ifd, ifd->n.last_rx_ring);
}

/* Get the index of the current or last tx or rx ring. */
uint16_t
tes_if_cur_txring_id (tes_ifdesc* ifd)
{
	return ifd->n.cur_tx_ring - ifd->n.first_tx_ring;
}
uint16_t
tes_if_last_txring_id (tes_ifdesc* ifd)
{
	return ifd->n.last_tx_ring - ifd->n.first_tx_ring;
}
uint16_t
tes_if_cur_rxring_id (tes_ifdesc* ifd)
{
	return ifd->n.cur_rx_ring - ifd->n.first_rx_ring;
}
uint16_t
tes_if_last_rxring_id (tes_ifdesc* ifd)
{
	return ifd->n.last_rx_ring - ifd->n.first_rx_ring;
}

/* Get the number of buffers in the ring. */
uint32_t
tes_ifring_bufs (tes_ifring* ring)
{
	return ring->n.num_slots;
}

/* Get the physical size of the buffers in the ring. */
uint32_t
tes_ifring_buf_size (tes_ifring* ring)
{
	return ring->n.nr_buf_size;
}

/* Compare slots mod num_slots taking into accout the ring's head.
 * Returns -1 or 1 if ida is closer or farther from the head than idb.
 * Returns 0 if they are equal. */
int
tes_ifring_compare_ids (tes_ifring* ring, uint32_t ida, uint32_t idb)
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
 * Returns the buf id that is closer (smaller) or farther (larger) to the
 * ring's head in a forward direction. */
uint32_t
tes_ifring_earlier_id (tes_ifring* ring, uint32_t ida, uint32_t idb)
{
	if (unlikely (ida == idb))
		return ida;

	if ( (ring->n.head <= ida && ring->n.head <= idb) ||
		(ring->n.head > ida && ring->n.head > idb) )
		return (ida < idb) ? ida : idb;

	return (ida < idb) ? idb : ida;
}
uint32_t
tes_ifring_later_id (tes_ifring* ring, uint32_t ida, uint32_t idb)
{
	if (unlikely (ida == idb))
		return ida;

	if ( (ring->n.head <= ida && ring->n.head <= idb) ||
		(ring->n.head > ida && ring->n.head > idb) )
		return (ida < idb) ? idb : ida;

	return (ida < idb) ? ida : idb;
}

/* Get the head, current, preceding <idx>, following <idx> or tail buffer id of
 * a ring. Wraps around. */
uint32_t
tes_ifring_head (tes_ifring* ring)
{
	return ring->n.head;
}
uint32_t
tes_ifring_cur (tes_ifring* ring)
{
	return ring->n.cur;
}
uint32_t
tes_ifring_preceding (tes_ifring* ring, uint32_t idx)
{
	return s_ring_preceding (ring, idx);
}
uint32_t
tes_ifring_following (tes_ifring* ring, uint32_t idx)
{
	return s_ring_following (ring, idx);
}
uint32_t
tes_ifring_tail (tes_ifring* ring)
{
	return ring->n.tail;
}

/* Get the current tx or rx ring's current buffer. */
char*
tes_if_cur_txbuf (tes_ifdesc* ifd)
{
	tes_ifring* ring = s_txring (ifd, ifd->n.cur_tx_ring);
	return s_buf (ring, ring->n.cur);
}
char*
tes_if_cur_rxbuf (tes_ifdesc* ifd)
{
	tes_ifring* ring = s_rxring (ifd, ifd->n.cur_rx_ring);
	return s_buf (ring, ring->n.cur);
}

/* Get the head, current, <idx>, preceding <idx>, following <idx> or tail
 * buffer of a ring. Wraps around.
 * preceding and following return NULL when reaching the head-1 or tail. */
char*
tes_ifring_head_buf (tes_ifring* ring)
{
	return s_buf (ring, ring->n.head);
}
char*
tes_ifring_cur_buf (tes_ifring* ring)
{
	return s_buf (ring, ring->n.cur);
}
char*
tes_ifring_buf (tes_ifring* ring, uint32_t idx)
{
	return s_buf (ring, idx);
}
char*
tes_ifring_preceding_buf (tes_ifring* ring, uint32_t idx)
{
	if (unlikely (idx == ring->n.head))
		return NULL;
	return s_buf (ring, s_ring_preceding (ring, idx));
}
char*
tes_ifring_following_buf (tes_ifring* ring, uint32_t idx)
{
	idx = s_ring_following (ring, idx);
	if (unlikely (idx == ring->n.tail))
		return NULL;
	return s_buf (ring, idx);
}
char*
tes_ifring_last_buf (tes_ifring* ring)
{
	return s_buf (ring, s_ring_preceding (ring, ring->n.tail));
}

/* Get the length of the current or <idx> buffer. */
uint16_t
tes_ifring_cur_len (tes_ifring* ring)
{
	return ring->n.slot[ ring->n.cur ].len;
}
uint16_t
tes_ifring_len (tes_ifring* ring, uint32_t idx)
{
	return ring->n.slot[ idx ].len;
}

/* Get the number of slots between cur and tail.
 * For rx rings, this is the number of uninspected received slots.
 * For tx rings, this is the number of free slots. */
uint32_t
tes_ifring_pending (tes_ifring* ring)
{
	if (ring->n.tail >= ring->n.cur)
		return ring->n.tail - ring->n.cur;
	return ring->n.num_slots + ring->n.tail - ring->n.cur;
}

/* Get the number of slots between head and cur.
 * For rx rings, this is the number of inspected received slots.
 * For tx rings, this is the number of received slots. */
uint32_t
tes_ifring_done (tes_ifring* ring)
{
	if (ring->n.cur >= ring->n.head)
		return ring->n.cur - ring->n.head;
	return ring->n.num_slots + ring->n.cur - ring->n.head;
}

/* Get the number of slots between head and tail.
 * For rx rings, this is the total number of received slots.
 * For tx rings, this is the number of received + free slots. */
uint32_t
tes_ifring_total (tes_ifring* ring)
{
	if (ring->n.tail >= ring->n.head)
		return ring->n.tail - ring->n.head;
	return ring->n.num_slots + ring->n.tail - ring->n.head;
}
