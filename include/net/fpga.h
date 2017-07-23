#ifndef __NET_FPGA_H_INCLUDED__
#define __NET_FPGA_H_INCLUDED__

#include <net/ethernet.h>
#include <sys/types.h>

#define FPGA_HDR_LEN 24 /* includes the 16 byte ethernet header */
#define MCA_HDR_LEN  40
#define TICK_HDR_LEN 24
#define PEAK_HDR_LEN  8
#define PEAK_LEN      8
#define PLS_LEN       8
#define PLS_HDR_LEN   8 + PLS_LEN
#define AREA_HDR_LEN  8
#define TR_HDR_LEN    8
#define TR_FULL_HDR_LEN   TR_HDR_LEN + PLS_LEN
#define DP_LEN        8
#define SMPL_LEN      2
#define BIN_LEN       4
#define MCA_FL_LEN    4
#define EVT_FL_LEN    2
#define TICK_FL_LEN   2
#define TR_FL_LEN     2
#define MAX_FPGA_FRAME_LEN  1496
#define MAX_MCA_FRAMES      45 /* will fit MAX_MCA_BINS + one MCA header and
				* some more*/
// #define MAX_TRACE_FRAMES
#define MAX_MCA_BINS_ALL    1 << 14
#define MAX_MCA_BINS_HFR      ((MAX_FPGA_FRAME_LEN - MCA_HDR_LEN - \
				FPGA_HDR_LEN) / BIN_LEN)
#define MAX_MCA_BINS_SFR      ((MAX_FPGA_FRAME_LEN - \
				FPGA_HDR_LEN) / BIN_LEN)
#define MAX_PLS_PEAKS         ((MAX_FPGA_FRAME_LEN - FPGA_HDR_LEN - \
				PLS_HDR_LEN) / PEAK_LEN)
#define MAX_TR_SGL_PEAKS_HFR  ((MAX_FPGA_FRAME_LEN - FPGA_HDR_LEN - \
				TR_FULL_HDR_LEN) / PEAK_LEN)
#define MAX_TR_SGL_SMPLS_HFR  ((MAX_FPGA_FRAME_LEN - FPGA_HDR_LEN - \
				TR_FULL_HDR_LEN) / SMPL_LEN)
#define MAX_TR_AVG_SMPLS_HFR  ((MAX_FPGA_FRAME_LEN - FPGA_HDR_LEN - \
				TR_HDR_LEN) / SMPL_LEN)
#define MAX_TR_DP_PEAKS_HFR   ((MAX_FPGA_FRAME_LEN - FPGA_HDR_LEN - \
				TR_FULL_HDR_LEN - DP_LEN) / PEAK_LEN)
#define MAX_TR_DPTR_PEAKS_HFR ((MAX_FPGA_FRAME_LEN - FPGA_HDR_LEN - \
				TR_FULL_HDR_LEN - DP_LEN) / PEAK_LEN)
#define MAX_TR_DPTR_SMPLS_HFR ((MAX_FPGA_FRAME_LEN - FPGA_HDR_LEN - \
				TR_FULL_HDR_LEN - DP_LEN) / SMPL_LEN)

#define ETH_EVT_TYPE     0x88B5
#define ETH_MCA_TYPE     0x88B6
#define EVT_TICK_TYPE    0x0002
#define EVT_PEAK_TYPE    0x0000
#define EVT_PLS_TYPE     0x0004
#define EVT_AREA_TYPE    0x0008
#define EVT_TR_SGL_TYPE  0x000c
#define EVT_TR_AVG_TYPE  0x010c
#define EVT_TR_DP_TYPE   0x020c
#define EVT_TR_DPTR_TYPE 0x030c

int fpgaerrno;

struct __mca_header
{
	u_int16_t size;
	u_int16_t last_bin;
	u_int32_t lowest_value;
	u_int16_t : 16; /* reserved */
	u_int16_t most_frequent;
	/* struct mca_flags flags; */
	u_int32_t flags;
	u_int64_t total;
	u_int64_t start_time;
	u_int64_t stop_time;
};

/* Used to access flags and time in an event-type agnostic way */
struct __evt_header
{
	u_int32_t : 32;
	u_int16_t flags;
	u_int16_t toff;
};

struct __tick_header
{
	u_int32_t period;
	u_int16_t flags;
	u_int16_t toff; /* time since last event */
	u_int64_t ts;   /* timestamp */
	u_int8_t ovrfl;
	u_int8_t err;
	u_int8_t cfd;
	u_int8_t : 8;   /* reserved */
	u_int32_t lost;
};

struct __peak_header
{
	u_int16_t height;
	u_int16_t rise_time;
	u_int16_t flags;
	u_int16_t toff;
};

struct __peak
{
	u_int16_t height;
	u_int16_t rise_time;
	u_int16_t minimum;
	u_int16_t toff;
};

struct __pulse
{
	u_int32_t area;
	u_int16_t length;
	u_int16_t toffset;
};

struct __pulse_header
{
	u_int16_t size;
	u_int16_t : 16; /* reserved */
	u_int16_t flags;
	u_int16_t toff;
	struct __pulse pulse;
};

struct __area_header
{
	u_int32_t area;
	u_int16_t flags;
	u_int16_t toff;
};

struct __trace_header
{
	u_int16_t size;
	u_int16_t tr_flags;
	u_int16_t flags;
	u_int16_t toff;
};

struct __trace_full_header
{
	struct __trace_header trace;
	struct __pulse pulse;
};

struct __dot_prod
{
	u_int16_t : 16; /* reserved */
	u_int64_t dot_prod : 48;
} __attribute__ ((__packed__));

struct __fpga_pkt
{
	struct
	{
		struct ether_header eth_hdr; /* packed, 14 bytes */
		u_int16_t length;	     /* length of packet */
	};
	struct
	{
		u_int16_t frame_seq;
		u_int16_t proto_seq;
		u_int16_t evt_size; /* undefined for MCA frames */
		u_int16_t evt_type; /* undefined for MCA frames */
	} fpga_hdr;
	u_char body[MAX_FPGA_FRAME_LEN - FPGA_HDR_LEN];
};

/* ------------------------------------------------------------------------- */
/* ---------------------------- INTERNAL DEBUG ----------------------------- */
/* ------------------------------------------------------------------------- */

#ifdef FPGA_DEBUG
#include <stdio.h>
#include <stddef.h>
#include <assert.h>

#define MCA_FL_MASK   0x000fffff
#define EVT_FL_MASK   0xffff
#define TICK_FL_MASK  0x0703
#define TR_FL_MASK    0x7fff

/* Return error codes */
#define FPGA_EETHTYPE 1 << 0 /* ether type */
#define FPGA_EFLEN    1 << 1 /* frame length */
#define FPGA_ESIZE    1 << 2 /* event size */
#define FPGA_ETYPE    1 << 3 /* event type */
#define FPGA_EFLAG    1 << 4 /* MCA, event or trace flag */
#define FPGA_ECLASH   1 << 5 /* contradiction between fields (packet type
			      * specific */
static void
__fpga_perror (FILE* out, const char* desc)
{
	if (fpgaerrno & FPGA_EETHTYPE)
		fprintf (out, "%s%sInvalid ethernet type\n",
			desc, desc[0] ? ": " : "");
	if (fpgaerrno & FPGA_EFLEN)
		fprintf (out, "%s%sInvalid packet length\n",
			desc, desc[0] ? ": " : "");
	if (fpgaerrno & FPGA_ESIZE)
		fprintf (out, "%s%sInvalid event size\n",
			desc, desc[0] ? ": " : "");
	if (fpgaerrno & FPGA_ETYPE)
		fprintf (out, "%s%sInvalid event type\n",
			desc, desc[0] ? ": " : "");
	if (fpgaerrno & FPGA_EFLAG)
		fprintf (out, "%s%sInvalid flags\n",
			desc, desc[0] ? ": " : "");
	if (fpgaerrno & FPGA_ECLASH)
		fprintf (out, "%s%sContradicting fields\n",
			desc, desc[0] ? ": " : "");
}

#if 0

static int
__check_fpga_pkt (struct __fpga_pkt* pkt)
{
	int rc = 0;

	/* Should we ensure reserved fields are zero? */

	/* Length should be a multiple of 8 */
	if ( pkt->length & 7 )
		rc |= FPGA_EFLEN;

	switch (pkt->eth_hdr.ether_type)
	{
	/* ------------------------------------ MCA -------------------------------- */
		case ETH_MCA_TYPE:
			if ( pkt->length < FPGA_HDR_LEN + BIN_LEN +
				(pkt->fpga_hdr.proto_seq ? 0 : MCA_HDR_LEN) )
				rc |= FPGA_EFLEN;
			
			if (pkt->fpga_hdr.proto_seq == 0)
			{
				struct __mca_header* mca_h =
					(struct __mca_header*) &pkt->body;

				if ( mca_h->flags & ~MCA_FL_MASK )
					rc |= FPGA_EFLAG;

				if ( mca_h->size != MCA_HDR_LEN +
						(mca_h->last_bin + 1)*BIN_LEN )
					rc |= FPGA_ECLASH;

				/* check lowest_value? */

				if (mca_h->most_frequent > mca_h->last_bin)
					rc |= FPGA_ECLASH;

				/* check start < stop time? */
			}

			break;

	/* ----------------------------------- Event ------------------------------- */
		case ETH_EVT_TYPE:
			;
			/*
			 * Events share a common structure, use switch
			 * fallthrough to set the parameters to be
			 * checked at the end
			 */
			u_int16_t evflag_mask = 0, /* EVT_FL_MASK except
						    * for tick events */
				  trflags = 0, /* only set by traces,
						* safe to leave at 0 */
				  minflen = FPGA_HDR_LEN, /* will be incremented */
				  maxflen = 0, /* to be set to either minflen
						* or to highest u_int16_t */
				  evsize = 0;  /* not implemented yet */
			switch (pkt->fpga_hdr.evt_type)
			{
				case EVT_TR_AVG_TYPE:
					minflen += TR_HDR_LEN;
				case EVT_TR_SGL_TYPE:
					if (minflen == FPGA_HDR_LEN)
						minflen += TR_FULL_HDR_LEN;
				case EVT_TR_DP_TYPE:
				case EVT_TR_DPTR_TYPE:
					if (minflen == FPGA_HDR_LEN)
						minflen += TR_FULL_HDR_LEN + DP_LEN;

					trflags = ((struct __trace_header*)
						&pkt->body)->tr_flags;
				/* ------------------- traces end ------------------ */
				case EVT_PLS_TYPE:
					if (minflen == FPGA_HDR_LEN)
						minflen += PLS_HDR_LEN;

					maxflen = (u_int16_t) -1;
				/* --------------- no max pkt len end -------------- */
				case EVT_PEAK_TYPE:
					if (minflen == FPGA_HDR_LEN)
						minflen += PEAK_HDR_LEN;
				case EVT_AREA_TYPE:
					if (minflen == FPGA_HDR_LEN)
						minflen += AREA_HDR_LEN;

					evflag_mask = EVT_FL_MASK;
				/* ---------------- EVT_FL_MASK end ---------------- */
				case EVT_TICK_TYPE:
					if (minflen == FPGA_HDR_LEN)
						minflen += TICK_HDR_LEN;
					if (maxflen == 0)
						maxflen = minflen;

					if (evflag_mask == 0)
						evflag_mask = TICK_FL_MASK;

			/* ------------------------- Checks ------------------------ */
					if (pkt->length < minflen || pkt->length > maxflen)
						rc |= FPGA_EFLEN;
					if ( ((struct __evt_header*) &pkt->body)->flags
						& ~evflag_mask )
						rc |= FPGA_EFLAG;
					if ( trflags & ~TR_FL_MASK )
						rc |= FPGA_EFLAG;
					break;
				default:
					rc |= FPGA_ETYPE;
					break;
			}
			break;

		default:
			rc |= FPGA_EETHTYPE;
			break;
	}
	return rc;
}

#else /* using more robust and clear code, possibly slower? */

typedef int (__clash_check_fn)(struct __fpga_pkt*);
static __clash_check_fn __check_mca_fields;

static const struct pkt_desc
{
	struct
	{ /* flags */
		union
		{
			u_int32_t mca;
			struct
			{
				u_int16_t trace; /* = 0xffff for non-trace events */
				u_int16_t event;
			};
		};
	} flmask;
	struct
	{ /* pkt length */
		u_int16_t min;
		u_int16_t max; /* = 0 if no max */
	} pktlen;
	__clash_check_fn* misc_chk; /* packet type specific */
	u_int16_t evsize; /* event size, = 0 if not fixed */
}
mca_hfr_desc = { /* header frame, i.e. proto seq = 0 */
	.flmask.mca = MCA_FL_MASK,
	.pktlen.min = FPGA_HDR_LEN + MCA_HDR_LEN + BIN_LEN,
	.misc_chk = __check_mca_fields,
	},
mca_sfr_desc = { /* sequence frame, i.e. proto seq > 0 */
	.flmask.mca = MCA_FL_MASK,
	.pktlen.min = FPGA_HDR_LEN + BIN_LEN,
	.misc_chk = __check_mca_fields,
	},
tick_desc = {
	.flmask.event = TICK_FL_MASK,
	.flmask.trace = 0xffff,
	.pktlen.min = FPGA_HDR_LEN + TICK_HDR_LEN,
	.pktlen.max = FPGA_HDR_LEN + TICK_HDR_LEN,
	.evsize = TICK_HDR_LEN >> 3, /* does it?? */
},
peak_desc = {
	.flmask.event = EVT_FL_MASK,
	.flmask.trace = 0xffff,
	.pktlen.min = FPGA_HDR_LEN + PEAK_HDR_LEN,
	.pktlen.max = FPGA_HDR_LEN + PEAK_HDR_LEN,
	.evsize = 1,  /* does it?? */
},
pulse_desc = {
	.flmask.event = EVT_FL_MASK,
	.flmask.trace = 0xffff,
	.pktlen.min = FPGA_HDR_LEN + PLS_HDR_LEN,
},
area_desc = {
	.flmask.event = EVT_FL_MASK,
	.flmask.trace = 0xffff,
	.pktlen.min = FPGA_HDR_LEN + AREA_HDR_LEN,
	.pktlen.max = FPGA_HDR_LEN + AREA_HDR_LEN,
	.evsize = 1,  /* does it?? */
},
trace_sgl_desc = {
	.flmask.event = EVT_FL_MASK,
	.flmask.trace = TR_FL_MASK,
	.pktlen.min = FPGA_HDR_LEN + TR_FULL_HDR_LEN,
	.evsize = 1,
},
trace_avg_desc = {
	.flmask.event = EVT_FL_MASK,
	.flmask.trace = TR_FL_MASK,
	.pktlen.min = FPGA_HDR_LEN + TR_HDR_LEN,
	.evsize = 1,
},
trace_dp_desc = {
	.flmask.event = EVT_FL_MASK,
	.flmask.trace = TR_FL_MASK,
	.pktlen.min = FPGA_HDR_LEN + TR_FULL_HDR_LEN + DP_LEN,
	.evsize = 1,
},
trace_dptr_desc = {
	.flmask.event = EVT_FL_MASK,
	.flmask.trace = TR_FL_MASK,
	.pktlen.min = FPGA_HDR_LEN + TR_FULL_HDR_LEN + DP_LEN,
	.evsize = 1,
};

static int
__check_mca_fields (struct __fpga_pkt* pkt)
{
	int rc = 0;
	return rc;
}

static int
__check_fpga_pkt (struct __fpga_pkt* pkt)
{
	int rc = 0;
	const struct pkt_desc* cur_desc = NULL;

	/* Should we ensure reserved fields are zero? */

	/* Check ether and possibly event type */
	switch (pkt->eth_hdr.ether_type)
	{
		case ETH_MCA_TYPE:
			if (pkt->fpga_hdr.proto_seq == 0)
				cur_desc = &mca_hfr_desc;
			else
				cur_desc = &mca_sfr_desc;
			break;
		case ETH_EVT_TYPE:
			switch (pkt->fpga_hdr.evt_type)
			{
				case EVT_PEAK_TYPE:
					cur_desc = &peak_desc;
					break;
				case EVT_TICK_TYPE:
					cur_desc = &tick_desc;
					break;
				case EVT_PLS_TYPE:
					cur_desc = &pulse_desc;
					break;
				case EVT_AREA_TYPE:
					cur_desc = &area_desc;
					break;
				case EVT_TR_SGL_TYPE:
					cur_desc = &trace_sgl_desc;
					break;
				case EVT_TR_AVG_TYPE:
					cur_desc = &trace_avg_desc;
					break;
				case EVT_TR_DP_TYPE:
					cur_desc = &trace_dp_desc;
					break;
				case EVT_TR_DPTR_TYPE:
					cur_desc = &trace_dptr_desc;
					break;
				default:
					return FPGA_ETYPE;
					break;
			}
			break;
		default:
			return FPGA_EETHTYPE;
			break;
	}

	/* Length should be a multiple of 8 */
	if ( pkt->length & 7 )
		rc |= FPGA_EFLEN;

	assert (cur_desc);
	if ( pkt->length < cur_desc->pktlen.min )
		rc |= FPGA_EFLEN;
	if ( cur_desc->pktlen.max &&
		pkt->length > cur_desc->pktlen.max )
		rc |= FPGA_EFLEN;
	if ( pkt->eth_hdr.ether_type == ETH_MCA_TYPE &&
		pkt->fpga_hdr.proto_seq == 0 )
	{
		if ( ((struct __mca_header*) &pkt->body)->flags
			& ~cur_desc->flmask.mca )
			rc |= FPGA_EFLAG;
	}
	else if (pkt->eth_hdr.ether_type == ETH_EVT_TYPE )
	{
		if ( ((struct __evt_header*) &pkt->body)->flags
			& ~cur_desc->flmask.event )
			rc |= FPGA_EFLAG;
		/* No problem to check trace flags even for non-trace events,
		 * since the mask will be set to 0xffff in this case */
		if ( ((struct __trace_header*) &pkt->body)->tr_flags
			& ~cur_desc->flmask.trace )
			rc |= FPGA_EFLAG;
	}

	return rc;
}

#endif

static void
__fpga_self_test ()
{
	assert (sizeof (struct __fpga_pkt) == MAX_FPGA_FRAME_LEN);
	assert (offsetof (struct __fpga_pkt, body) == FPGA_HDR_LEN);
	assert (sizeof (struct __mca_header) == MCA_HDR_LEN);
	assert (sizeof (struct __tick_header) == TICK_HDR_LEN);
	assert (sizeof (struct __peak_header) == PEAK_HDR_LEN);
	assert (sizeof (struct __peak) == PEAK_LEN);
	assert (sizeof (struct __pulse) == PLS_LEN);
	assert (sizeof (struct __pulse_header) == PLS_HDR_LEN);
	assert (sizeof (struct __area_header) == AREA_HDR_LEN);
	assert (sizeof (struct __trace_header) == TR_HDR_LEN);
	assert (sizeof (struct __trace_full_header) == TR_FULL_HDR_LEN);
	assert (sizeof (struct __dot_prod) == DP_LEN);
#ifdef __NET_FPGA_USER_H_INCLUDED__
	__fpga_user_self_test ();
#endif
}

#endif /* FPGA_DEBUG */

#endif
