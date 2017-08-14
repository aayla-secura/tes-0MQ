#ifndef __NET_FPGA_H_INCLUDED__
#define __NET_FPGA_H_INCLUDED__

#include <net/ethernet.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <assert.h>

/* ------------------------------------------------------------------------- */

typedef struct fpga_pkt fpga_pkt;

int is_header (fpga_pkt* pkt);
int is_mca (fpga_pkt* pkt);
int is_evt (fpga_pkt* pkt);
int is_tick (fpga_pkt* pkt);
int is_peak (fpga_pkt* pkt);
int is_pulse (fpga_pkt* pkt);
int is_area (fpga_pkt* pkt);
int is_trace (fpga_pkt* pkt);
int is_trace_sgl (fpga_pkt* pkt);
int is_trace_avg (fpga_pkt* pkt);
int is_trace_dp (fpga_pkt* pkt);
int is_trace_dptr (fpga_pkt* pkt);
uint16_t pkt_len (fpga_pkt* pkt);
uint16_t frame_seq (fpga_pkt* pkt);
uint16_t proto_seq (fpga_pkt* pkt);
uint32_t mca_bin (fpga_pkt* pkt, uint16_t bin);
uint32_t mca_flags (fpga_pkt* pkt);
uint16_t evt_flags (fpga_pkt* pkt);
uint16_t trace_flags (fpga_pkt* pkt);
uint16_t evt_toff (fpga_pkt* pkt);
void pkt_pretty_print (fpga_pkt* pkt, FILE* stream);

#ifdef FPGAPKT_DEBUG
void fpga_pkt_self_test (void);
#endif

/*
 * You can copy a flag integer into one of these structures to read off the
 * separate registers.
 */
struct mca_flags
{
#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t   :  8; /* reserved */

	uint8_t Q :  4;
	uint8_t   :  4; /* reserved */

	uint8_t T :  4;
	uint8_t V :  4;

	uint8_t C :  3;
	uint8_t N :  5;
#else
	uint8_t   :  8; /* reserved */

	uint8_t   :  4; /* reserved */
	uint8_t Q :  4;

	uint8_t V :  4;
	uint8_t T :  4;

	uint8_t N :  5;
	uint8_t C :  3;
#endif
};

struct event_flags
{
#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t CH : 3;
	uint8_t O  : 1;
	uint8_t PC : 4;

	uint8_t N  : 1;
	uint8_t T  : 1;
	uint8_t PT : 2;
	uint8_t HT : 2;
	uint8_t TT : 2;
#else
	uint8_t PC : 4;
	uint8_t O  : 1;
	uint8_t CH : 3;

	uint8_t TT : 2;
	uint8_t HT : 2;
	uint8_t PT : 2;
	uint8_t T  : 1;
	uint8_t N  : 1;
#endif
};

struct tick_flags
{
#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t TL : 1;
	uint8_t EL : 1;
	uint8_t MF : 1;
	uint8_t    : 5; /* reserved */

	uint8_t N  : 1;
	uint8_t T  : 1;
	uint8_t    : 6; /* reserved */
#else
	uint8_t    : 5; /* reserved */
	uint8_t MF : 1;
	uint8_t EL : 1;
	uint8_t TL : 1;

	uint8_t    : 6; /* reserved */
	uint8_t T  : 1;
	uint8_t N  : 1;
#endif
};

struct trace_flags
{
#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t STR : 5;
	uint8_t MP  : 1;
	uint8_t MH  : 1;
	uint8_t     : 1; /* reserved */

	uint8_t OFF : 4;
	uint8_t TS  : 2;
	uint8_t TT  : 2;
#else
	uint8_t     : 1; /* reserved */
	uint8_t MH  : 1;
	uint8_t MP  : 1;
	uint8_t STR : 5;

	uint8_t TT  : 2;
	uint8_t TS  : 2;
	uint8_t OFF : 4;
#endif
};

/* ------------------------------------------------------------------------- */

#define FPGA_HDR_LEN 24 /* includes the 16 byte ethernet header */
#define MCA_HDR_LEN  40
#define TICK_HDR_LEN 24
#define PEAK_HDR_LEN  8
#define PEAK_LEN      8
#define PLS_LEN       8
#define PLS_HDR_LEN   (8 + PLS_LEN)
#define AREA_HDR_LEN  8
#define TR_HDR_LEN    8
#define TR_FULL_HDR_LEN   (TR_HDR_LEN + PLS_LEN)
#define DP_LEN        8
#define SMPL_LEN      2
#define BIN_LEN       4
#define MCA_FL_LEN    4
#define EVT_FL_LEN    2
#define TICK_FL_LEN   2
#define TR_FL_LEN     2
#define MAX_FPGA_FRAME_LEN  1496

#if __BYTE_ORDER == __LITTLE_ENDIAN
#  define ETH_EVT_TYPE     0xB588
#  define ETH_MCA_TYPE     0xB688
#  define EVT_TICK_TYPE    0x0200
#  define EVT_PEAK_TYPE    0x0000
#  define EVT_PLS_TYPE     0x0400
#  define EVT_AREA_TYPE    0x0800
#  define EVT_TR_SGL_TYPE  0x0c00
#  define EVT_TR_AVG_TYPE  0x0c01
#  define EVT_TR_DP_TYPE   0x0c02
#  define EVT_TR_DPTR_TYPE 0x0c03
#  define EVT_TR_TYPE_MASK 0x0f00 /* the first 4 bits give the packet type */
#  define EVT_TR_TYPE      0x0c00
#else
#  define ETH_EVT_TYPE     0x88B5
#  define ETH_MCA_TYPE     0x88B6
#  define EVT_TICK_TYPE    0x0002
#  define EVT_PEAK_TYPE    0x0000
#  define EVT_PLS_TYPE     0x0004
#  define EVT_AREA_TYPE    0x0008
#  define EVT_TR_SGL_TYPE  0x000c
#  define EVT_TR_AVG_TYPE  0x010c
#  define EVT_TR_DP_TYPE   0x020c
#  define EVT_TR_DPTR_TYPE 0x030c
#  define EVT_TR_TYPE_MASK 0x000f /* the first 4 bits give the packet type */
#  define EVT_TR_TYPE      0x000c
#endif

struct mca_header
{
	uint16_t size;
	uint16_t last_bin;
	uint32_t lowest_value;
	uint16_t : 16; /* reserved */
	uint16_t most_frequent;
	/* struct mca_flags flags; */
	uint32_t flags;
	uint64_t total;
	uint64_t start_time;
	uint64_t stop_time;
};

/* Used to access flags and time in an event-type agnostic way */
struct evt_header
{
	uint32_t : 32;
	uint16_t flags;
	uint16_t toff;
};

struct tick_header
{
	uint32_t period;
	uint16_t flags;
	uint16_t toff; /* time since last event */
	uint64_t ts;   /* timestamp */
	uint8_t  ovrfl;
	uint8_t  err;
	uint8_t  cfd;
	uint8_t  : 8;   /* reserved */
	uint32_t lost;
};

struct peak_header
{
	uint16_t height;
	uint16_t rise_time;
	uint16_t flags;
	uint16_t toff;
};

struct peak
{
	uint16_t height;
	uint16_t rise_time;
	uint16_t minimum;
	uint16_t toff;
};

struct pulse
{
	uint32_t area;
	uint16_t length;
	uint16_t toffset;
};

struct pulse_header
{
	uint16_t size;
	uint16_t : 16; /* reserved */
	uint16_t flags;
	uint16_t toff;
	struct   pulse pulse;
};

struct area_header
{
	uint32_t area;
	uint16_t flags;
	uint16_t toff;
};

struct trace_header
{
	uint16_t size;
	uint16_t tr_flags;
	uint16_t flags;
	uint16_t toff;
};

struct trace_full_header
{
	struct trace_header trace;
	struct pulse pulse;
};

struct dot_prod
{
	uint16_t : 16; /* reserved */
	uint64_t dot_prod : 48;
} __attribute__ ((__packed__));

struct fpga_pkt
{
	struct
	{
		struct ether_header eth_hdr; /* packed, 14 bytes */
		uint16_t length;	     /* length of packet */
	};
	struct
	{
		uint16_t frame_seq;
		uint16_t proto_seq;
		uint16_t evt_size; /* undefined for MCA frames */
		uint16_t evt_type; /* undefined for MCA frames */
	} fpga_hdr;
	char body[MAX_FPGA_FRAME_LEN - FPGA_HDR_LEN];
};

/* ------------------------------------------------------------------------- */

int
is_header (fpga_pkt* pkt)
{
	return ( pkt->fpga_hdr.proto_seq == 0 );
}

int
is_mca (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_MCA_TYPE );
}

int
is_evt (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE );
}

int
is_tick (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE &&
		 pkt->fpga_hdr.evt_type == EVT_TICK_TYPE );
}

int
is_peak (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE &&
		 pkt->fpga_hdr.evt_type == EVT_PEAK_TYPE );
}

int
is_pulse (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE &&
		 pkt->fpga_hdr.evt_type == EVT_PLS_TYPE );
}

int
is_area (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE &&
		 pkt->fpga_hdr.evt_type == EVT_AREA_TYPE );
}

int
is_trace (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE &&
		(pkt->fpga_hdr.evt_type & EVT_TR_TYPE_MASK)
		 		       == EVT_TR_TYPE );
}

int
is_trace_sgl (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE &&
		 pkt->fpga_hdr.evt_type == EVT_TR_SGL_TYPE );
}

int
is_trace_avg (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE &&
		 pkt->fpga_hdr.evt_type == EVT_TR_AVG_TYPE );
}

int
is_trace_dp (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE &&
		 pkt->fpga_hdr.evt_type == EVT_TR_DP_TYPE );
}

int
is_trace_dptr (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE &&
		 pkt->fpga_hdr.evt_type == EVT_TR_DPTR_TYPE );
}

uint16_t
pkt_len (fpga_pkt* pkt)
{
	return pkt->length;
	// return ntohs (pkt->length);
}

uint16_t
frame_seq (fpga_pkt* pkt)
{
	return pkt->fpga_hdr.frame_seq;
	// return ntohs (pkt->fpga_hdr.frame_seq);
}

uint16_t
proto_seq (fpga_pkt* pkt)
{
	return pkt->fpga_hdr.proto_seq;
	// return ntohs (pkt->fpga_hdr.proto_seq);
}

uint32_t
mca_bin (fpga_pkt* pkt, uint16_t bin)
{
	if (is_header (pkt))
		return pkt->body[ bin*BIN_LEN + MCA_HDR_LEN ];
		// return ntohl (pkt->body[ bin*BIN_LEN + MCA_HDR_LEN ]);
	else
		return pkt->body[ bin*BIN_LEN ];
		// return ntohl (pkt->body[ bin*BIN_LEN ]);
}

uint32_t
mca_flags (fpga_pkt* pkt)
{
	return ((struct mca_header*)(void*) &pkt->body)->flags;
	// return ntohl (((struct mca_header*)(void*) &pkt->body)->flags);
}

uint16_t
evt_flags (fpga_pkt* pkt)
{
	return ((struct evt_header*)(void*) &pkt->body)->flags;
	// return ntohs (((struct evt_header*)(void*) &pkt->body)->flags);
}

uint16_t
trace_flags (fpga_pkt* pkt)
{
	return ((struct trace_header*)(void*) &pkt->body)->tr_flags;
	// return ntohs (((struct trace_header*)(void*) &pkt->body)->tr_flags);
}

uint16_t
evt_toff (fpga_pkt* pkt)
{
	return ((struct evt_header*)(void*) &pkt->body)->toff;
	// return ntohs (((struct evt_header*)(void*) &pkt->body)->toff);
}

#if 0
// TO DO
void
pkt_pretty_print (fpga_pkt* pkt, FILE* stream)
{
	fprintf (stream, "Source MAC:          \n", );
	fprintf (stream, "Destination MAC:     \n", );
	fprintf (stream, "Packet length:       \n", );
	fprintf (stream, "Frame sequence:      \n", );
	fprintf (stream, "Protocol sequence:   \n", );

	/* ----- MCA */
	if (is_mca (pkt))
	{
		fprintf (stream, "Stream type:         MCA\n");
		fprintf (stream, "Size:                \n", );
		fprintf (stream, "Flag Q:              \n", );
		fprintf (stream, "Flag V:              \n", );
		fprintf (stream, "Flag T:              \n", );
		fprintf (stream, "Flag N:              \n", );
		fprintf (stream, "Flag C:              \n", );
		fprintf (stream, "Lowest value:        \n", );
		fprintf (stream, "Last bin:            \n", );
		fprintf (stream, "Most frequent bin:   \n", );
		fprintf (stream, "Total:               \n", );
		fprintf (stream, "Start time:          \n", );
		fprintf (stream, "Stop time:           \n", );
		return;
	}
	if (!is_evt (pkt))
	{
		fprintf (stream, "Unknown stream type\n");
		return;
	}

	/* ----- Event */
	fprintf (stream, "Stream type:         Event\n");
	fprintf (stream, "Event size:          \n", );
	fprintf (stream, "Time offset:         \n", );
	/* ---------- Tick */
	if (is_tick (pkt))
	{
		fprintf (stream, "Type:                Tick\n");
		fprintf (stream, "Tick flag MF:        \n", );
		fprintf (stream, "Tick flag EL:        \n", );
		fprintf (stream, "Tick flag TL:        \n", );
		fprintf (stream, "Tick flag T:         \n", );
		fprintf (stream, "Tick flag N:         \n", );
		fprintf (stream, "Period:              \n", );
		fprintf (stream, "Timestamp:           \n", );
		fprintf (stream, "Error ovrfl:         \n", );
		fprintf (stream, "Error err:           \n", );
		fprintf (stream, "Error cfd:           \n", );
		fprintf (stream, "Events lost:         \n", );
		return;
	}
	/* ---------- Measurement */
	fprintf (stream, "Event flag PC:       \n", );
	fprintf (stream, "Event flag O:        \n", );
	fprintf (stream, "Event flag CH:       \n", );
	fprintf (stream, "Event flag TT:       \n", );
	fprintf (stream, "Event flag HT:       \n", );
	fprintf (stream, "Event flag PA:       \n", );
	fprintf (stream, "Event flag T:        \n", );
	fprintf (stream, "Event flag N:        \n", );
	/* --------------- Peak */
	if (is_peak (pkt))
	{
		fprintf (stream, "Type:                Peak\n");
		fprintf (stream, "Height:              \n", );
		fprintf (stream, "Rise time:           \n", );
		return;
	}
	/* --------------- Area */
	if (is_area (pkt))
	{
		fprintf (stream, "Type:                Area\n");
		fprintf (stream, "Area:                \n", );
		return;
	}
	fprintf (stream, "Size:                        \n", );
	/* --------------- Pulse */
	if (is_pulse (pkt))
	{
		fprintf (stream, "Type:                Pulse\n");
		fprintf (stream, "Area:                \n", );
		fprintf (stream, "Length:              \n", );
		fprintf (stream, "Time offset:         \n", );
		return;
	}
	if (!is_trace (pkt))
	{
		fprintf (stream, "Unknown event type\n");
		return;
	}
	/* --------------- Trace */
	fprintf (stream, "Trace flag MH:       \n", );
	fprintf (stream, "Trace flag MP:       \n", );
	fprintf (stream, "Trace flag STR:      \n", );
	fprintf (stream, "Trace flag TT:       \n", );
	fprintf (stream, "Trace flag TS:       \n", );
	fprintf (stream, "Trace flag OFF:      \n", );
	/* -------------------- Average */
	if (is_trace_avg (pkt))
	{
		fprintf (stream, "Type:                Average\n");
		return;
	}
	fprintf (stream, "Area:                \n", );
	fprintf (stream, "Length:              \n", );
	fprintf (stream, "Time offset:         \n", );
	/* -------------------- Single */
	if (is_trace_sgl (pkt))
	{
		fprintf (stream, "Type:                Single\n");
		return;
	}
	/* -------------------- Dot product */
	if (is_trace_dp (pkt))
	{
		fprintf (stream, "Type:                Dot product\n");
		fprintf (stream, "Dot product:         \n", );
		return;
	}
	/* -------------------- Dot product + trace */
	if (is_trace_dptr (pkt))
	{
		fprintf (stream, "Type:                Dot product with trace\n");
		fprintf (stream, "Dot product:         \n", );
		return;
	}
	fprintf (stream, "Unknown trace type\n");
}
#endif

/* ------------------------------------------------------------------------- */
/* --------------------------------- DEBUG --------------------------------- */
/* ------------------------------------------------------------------------- */

#ifdef FPGAPKT_DEBUG

#if __BYTE_ORDER == __LITTLE_ENDIAN
#  define MCA_FL_MASK   0x00f0ffff
#  define EVT_FL_MASK   0xffff
#  define TICK_FL_MASK  0xe0c0
#  define TR_FL_MASK    0xfeff
#else
#  define MCA_FL_MASK   0x000fffff
#  define EVT_FL_MASK   0xffff
#  define TICK_FL_MASK  0x0703
#  define TR_FL_MASK    0x7fff
#endif
#define fl_str_to_u(fl_str_p)  *( (uint32_t*) (void*) fl_str_p )
#define fl_str_to_us(fl_str_p) *( (uint16_t*) (void*) fl_str_p )

void
fpgapkt_self_test (void)
{
	assert (sizeof (fpga_pkt) == MAX_FPGA_FRAME_LEN);
	assert (offsetof (fpga_pkt, body) == FPGA_HDR_LEN);
	assert (sizeof (struct mca_header) == MCA_HDR_LEN);
	assert (sizeof (struct tick_header) == TICK_HDR_LEN);
	assert (sizeof (struct peak_header) == PEAK_HDR_LEN);
	assert (sizeof (struct peak) == PEAK_LEN);
	assert (sizeof (struct pulse) == PLS_LEN);
	assert (sizeof (struct pulse_header) == PLS_HDR_LEN);
	assert (sizeof (struct area_header) == AREA_HDR_LEN);
	assert (sizeof (struct trace_header) == TR_HDR_LEN);
	assert (sizeof (struct trace_full_header) == TR_FULL_HDR_LEN);
	assert (sizeof (struct dot_prod) == DP_LEN);

	assert (sizeof (struct mca_flags) == MCA_FL_LEN);
	assert (sizeof (struct event_flags) == EVT_FL_LEN);
	assert (sizeof (struct tick_flags) == TICK_FL_LEN);
	assert (sizeof (struct trace_flags) == TR_FL_LEN);
	struct mca_flags mf = {0,};
	mf.C = 0x07;
	mf.N = 0x1f;
	mf.T = 0x0f;
	mf.V = 0x0f;
	mf.Q = 0x0f;
	assert ( fl_str_to_u (&mf) == MCA_FL_MASK );
	struct event_flags ef = {0,};
	ef.N  = 0x01;
	ef.T  = 0x01;
	ef.PT = 0x03;
	ef.HT = 0x03;
	ef.TT = 0x03;
	ef.CH = 0x07;
	ef.O  = 0x01;
	ef.PC = 0x0f;
	assert ( fl_str_to_us(&ef) == EVT_FL_MASK );
	struct tick_flags tf = {0,};
	tf.N  = 0x01;
	tf.T  = 0x01;
	tf.TL = 0x01;
	tf.EL = 0x01;
	tf.MF = 0x01;
	assert ( fl_str_to_us(&tf) == TICK_FL_MASK );
	struct trace_flags trf = {0,};
	trf.OFF = 0x0f;
	trf.TS  = 0x03;
	trf.TT  = 0x03;
	trf.STR = 0x1f;
	trf.MP  = 0x01;
	trf.MH  = 0x01;
	assert ( fl_str_to_us(&trf) == TR_FL_MASK );
}

#endif /* FPGAPKT_DEBUG */

#endif
