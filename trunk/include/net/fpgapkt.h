#ifndef __NET_FPGA_H_INCLUDED__
#define __NET_FPGA_H_INCLUDED__

/* Set if the FPGA frame payload matches that of the host */
#define KEEP_BYTEORDER

#include <net/ethernet.h>
#ifdef linux
/* ntoa, aton; on FreeBSD these are provided by net/ethernet */
#  include <netinet/ether.h>
#endif /* linux */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <assert.h>

/* ------------------------------------------------------------------------- */

typedef struct fpga_pkt fpga_pkt;

/* Destination and source MAC addresses */
static inline char* dst_eth_ntoa (fpga_pkt* pkt);
static inline char* src_eth_ntoa (fpga_pkt* pkt);
static inline struct ether_addr* dst_eth_aton (fpga_pkt* pkt);
static inline struct ether_addr* src_eth_aton (fpga_pkt* pkt);
/* Frame length (including ethernet header) */
static inline uint16_t pkt_len   (fpga_pkt* pkt);
/* Frame and protocol sequences */
static inline uint16_t frame_seq (fpga_pkt* pkt);
static inline uint16_t proto_seq (fpga_pkt* pkt);
/* True or false if frame is any header frame (protocol sequence == 0) */
static inline int is_header (fpga_pkt* pkt);
/* True or false if frame is any MCA or any event frame (the two are
 * complementary) */
static inline int is_mca   (fpga_pkt* pkt);
static inline int is_evt   (fpga_pkt* pkt);
/* True or false if frame is a tick, peak, area or pulse event */
static inline int is_tick  (fpga_pkt* pkt);
static inline int is_peak  (fpga_pkt* pkt);
static inline int is_area  (fpga_pkt* pkt);
static inline int is_pulse (fpga_pkt* pkt);
/* True or false if frame is any trace event */
static inline int is_trace (fpga_pkt* pkt);
/* True or false if frame is a single trace event */
static inline int is_trace_sgl  (fpga_pkt* pkt);
/* True or false if frame is a single, average, dot-product or
 * trace-dot-product trace event */
static inline int is_trace_avg  (fpga_pkt* pkt);
static inline int is_trace_dp   (fpga_pkt* pkt);
static inline int is_trace_dptr (fpga_pkt* pkt);
/* Event's size (valid for all events) */
static inline uint16_t evt_size (fpga_pkt* pkt);
/* Size of histogram (valid for MCA header frames) */
static inline uint16_t mca_size (fpga_pkt* pkt);
/* Number of bins in this frame (valid for all MCA frames) */
static inline uint16_t mca_num_bins (fpga_pkt* pkt);
/* Number of bins in entire histogram (valid for MCA header frames) */
static inline uint16_t mca_num_allbins (fpga_pkt* pkt);
/* Histogram's lowest value, most frequent bin, sum of all bins, start and stop
 * timestamp (valid for MCA header frames) */
static inline uint32_t mca_lvalue (fpga_pkt* pkt);
static inline uint16_t mca_mfreq  (fpga_pkt* pkt);
static inline uint64_t mca_total  (fpga_pkt* pkt);
static inline uint64_t mca_startt (fpga_pkt* pkt);
static inline uint64_t mca_stopt  (fpga_pkt* pkt);
/* Get bin number <bin> (starting at 0) of the current frame */
static inline uint32_t mca_bin (fpga_pkt* pkt, uint16_t bin);
/* MCA flags as a struct with separate fields for each register (valid for all
 * MCA frames) */
static inline struct mca_flags*   mca_fl  (fpga_pkt* pkt);
/* Event (or tick) flags as a struct with separate fields for each register
 * (valid for all event frames) */
static inline struct event_flags* evt_fl  (fpga_pkt* pkt);
static inline struct tick_flags*  tick_fl (fpga_pkt* pkt);
/* Trace flags as a struct with separate fields for each register  (valid for
 * trace events) */
static inline struct trace_flags* trace_fl (fpga_pkt* pkt);
/* Event's time (valid for all events) */
static inline uint16_t evt_toff (fpga_pkt* pkt);
/* Tick's period, timestamp, error registers and events lost (valid for tick
 * events) */
static inline uint32_t tick_period (fpga_pkt* pkt);
static inline uint64_t tick_ts     (fpga_pkt* pkt);
static inline uint8_t  tick_ovrfl  (fpga_pkt* pkt);
static inline uint8_t  tick_err    (fpga_pkt* pkt);
static inline uint8_t  tick_cfd    (fpga_pkt* pkt);
static inline uint32_t tick_lost   (fpga_pkt* pkt);
/* Peak's height and rise time (valid for peak events)  */
static inline uint16_t peak_ht    (fpga_pkt* pkt);
static inline uint16_t peak_riset (fpga_pkt* pkt);
/* Area's area (valid for area events) */
static inline uint32_t area_area  (fpga_pkt* pkt);
/* FIX: Pulse's size, area, length, time offset (valid for pulse events) */
static inline uint16_t pulse_size (fpga_pkt* pkt);
static inline uint32_t pulse_area (fpga_pkt* pkt);
static inline uint16_t pulse_len  (fpga_pkt* pkt);
static inline uint16_t pulse_toff (fpga_pkt* pkt);
/* FIX: Trace's size (valid for all trace events except average) */
static inline uint16_t trace_size (fpga_pkt* pkt);
/* FIX: Trace's area, length, time offset (valid for all trace events) */
static inline uint32_t trace_area (fpga_pkt* pkt);
static inline uint16_t trace_len  (fpga_pkt* pkt);
static inline uint16_t trace_toff (fpga_pkt* pkt);
/* Print info about packet */
static void pkt_pretty_print (fpga_pkt* pkt, FILE* stream);
/* Check if packet is valid, returns 0 if all is ok, or one or more OR-ed flags */
static int  is_valid (fpga_pkt* pkt);
/* Print info about each of the flags present in the return value of is_valid */
static void pkt_perror (FILE* stream, int err);

#ifdef FPGAPKT_DEBUG
static void fpga_pkt_self_test (void);
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
#define AREA_HDR_LEN  8
#define PLS_LEN       8
#define PLS_HDR_LEN  16 // 8 + PLS_LEN
#define TR_HDR_LEN    8
#define TR_FULL_HDR_LEN 16 // TR_HDR_LEN + PLS_LEN
#define DP_LEN        8
#define SMPL_LEN      2
#define BIN_LEN       4
#define MCA_FL_LEN    4
#define EVT_FL_LEN    2
#define TR_FL_LEN     2
#define MAX_FPGA_FRAME_LEN  1496

/*
 * Redefine the ether and event types for little-endian hosts, instead of using
 * ntohl/s, since we never return those as 16-bit integers (only used in is_*
 * helpers).
 */
#if __BYTE_ORDER == __LITTLE_ENDIAN
#  define ETH_EVT_TYPE      0xB588
#  define ETH_MCA_TYPE      0xB688
#  define EVT_TYPE_MASK     0x0e03 /* all relevant bits of evt_type */
#  define EVT_PKT_TYPE_MASK 0x0e00 /* the packet type and tick bits */
#  define EVT_TICK_TYPE     0x0200
#  define EVT_PEAK_TYPE     0x0000
#  define EVT_AREA_TYPE     0x0400
#  define EVT_PLS_TYPE      0x0800
#  define EVT_TR_TYPE       0x0c00
#  define EVT_TR_SGL_TYPE   0x0c00
#  define EVT_TR_AVG_TYPE   0x0c01
#  define EVT_TR_DP_TYPE    0x0c02
#  define EVT_TR_DPTR_TYPE  0x0c03
#else
#  define ETH_EVT_TYPE      0x88B5
#  define ETH_MCA_TYPE      0x88B6
#  define EVT_TYPE_MASK     0x030e /* all relevant bits of evt_type */
#  define EVT_PKT_TYPE_MASK 0x000e /* the packet type and tick bits */
#  define EVT_TICK_TYPE     0x0002
#  define EVT_PEAK_TYPE     0x0000
#  define EVT_AREA_TYPE     0x0004
#  define EVT_PLS_TYPE      0x0008
#  define EVT_TR_TYPE       0x000c
#  define EVT_TR_SGL_TYPE   0x000c
#  define EVT_TR_AVG_TYPE   0x010c
#  define EVT_TR_DP_TYPE    0x020c
#  define EVT_TR_DPTR_TYPE  0x030c
#endif

#define MCA_FL_MASK       0x000fffff
#define EVT_FL_MASK       0xffff
#define TICK_FL_MASK      0x0703
#define TR_FL_MASK        0x7fff

struct mca_header
{
	uint16_t size;
	uint16_t last_bin;
	uint32_t lowest_value;
	uint16_t : 16; /* reserved */
	uint16_t most_frequent;
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

struct area_header
{
	uint32_t area;
	uint16_t flags;
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

static inline int
is_header (fpga_pkt* pkt)
{ /* Byte order is irrelevant */
	return ( pkt->fpga_hdr.proto_seq == 0 );
}

/* Ethernet type is always big-endian */
static inline int
is_mca (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_MCA_TYPE );
}

static inline int
is_evt (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE );
}

static inline int
is_tick (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE &&
		 pkt->fpga_hdr.evt_type & EVT_TICK_TYPE );
}

static inline int
is_peak (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE &&
		(pkt->fpga_hdr.evt_type & EVT_PKT_TYPE_MASK)
			== EVT_PEAK_TYPE );
}

static inline int
is_pulse (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE &&
		(pkt->fpga_hdr.evt_type & EVT_PKT_TYPE_MASK)
			== EVT_PLS_TYPE );
}

static inline int
is_area (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE &&
		(pkt->fpga_hdr.evt_type & EVT_PKT_TYPE_MASK)
			== EVT_AREA_TYPE );
}

static inline int
is_trace (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE &&
		(pkt->fpga_hdr.evt_type & EVT_PKT_TYPE_MASK)
			== EVT_TR_TYPE );
}

static inline int
is_trace_sgl (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE &&
		(pkt->fpga_hdr.evt_type & EVT_TYPE_MASK)
			== EVT_TR_SGL_TYPE );
}

static inline int
is_trace_avg (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE &&
		(pkt->fpga_hdr.evt_type & EVT_TYPE_MASK)
			== EVT_TR_AVG_TYPE );
}

static inline int
is_trace_dp (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE &&
		(pkt->fpga_hdr.evt_type & EVT_TYPE_MASK)
			== EVT_TR_DP_TYPE );
}

static inline int
is_trace_dptr (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_EVT_TYPE &&
		(pkt->fpga_hdr.evt_type & EVT_TYPE_MASK)
			== EVT_TR_DPTR_TYPE );
}

static inline char*
dst_eth_ntoa (fpga_pkt* pkt)
{
	return ether_ntoa ((struct ether_addr*) pkt->eth_hdr.ether_dhost);
}

static inline char*
src_eth_ntoa (fpga_pkt* pkt)
{
	return ether_ntoa ((struct ether_addr*) pkt->eth_hdr.ether_shost);
}

static inline struct ether_addr*
dst_eth_aton (fpga_pkt* pkt)
{
	return ((struct ether_addr*) pkt->eth_hdr.ether_dhost);
}

static inline struct ether_addr*
src_eth_aton (fpga_pkt* pkt)
{
	return ((struct ether_addr*) pkt->eth_hdr.ether_shost);
}

static inline uint16_t
pkt_len (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return pkt->length;
#else
	return ntohs (pkt->length);
#endif
}

static inline uint16_t
frame_seq (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return pkt->fpga_hdr.frame_seq;
#else
	return ntohs (pkt->fpga_hdr.frame_seq);
#endif
}

static inline uint16_t
proto_seq (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return pkt->fpga_hdr.proto_seq;
#else
	return ntohs (pkt->fpga_hdr.proto_seq);
#endif
}

static inline uint16_t
evt_size (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return pkt->fpga_hdr.evt_size;
#else
	return ntohs (pkt->fpga_hdr.evt_size);
#endif
}

static inline uint16_t
mca_size (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct mca_header*)(void*) &pkt->body)->size;
#else
	return ntohl (((struct mca_header*)(void*) &pkt->body)->size);
#endif
}

static inline uint16_t
mca_num_bins (fpga_pkt* pkt)
{
	if (is_header (pkt))
		return ( (pkt_len (pkt) - FPGA_HDR_LEN - MCA_HDR_LEN )
			/ BIN_LEN );
	else
		return ( (pkt_len (pkt) - FPGA_HDR_LEN) / BIN_LEN );
}

static inline uint16_t
mca_num_allbins (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct mca_header*)(void*) &pkt->body)->last_bin + 1;
#else
	return ntohl (((struct mca_header*)(void*) &pkt->body)->last_bin + 1);
#endif
}

static inline uint32_t
mca_lvalue (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct mca_header*)(void*) &pkt->body)->lowest_value;
#else
	return ntohl (((struct mca_header*)(void*) &pkt->body)->lowest_value);
#endif
}

static inline uint16_t
mca_mfreq (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct mca_header*)(void*) &pkt->body)->most_frequent;
#else
	return ntohl (((struct mca_header*)(void*) &pkt->body)->most_frequent);
#endif
}

static inline uint64_t
mca_total (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct mca_header*)(void*) &pkt->body)->total;
#else
	return ntohl (((struct mca_header*)(void*) &pkt->body)->total);
#endif
}

static inline uint64_t
mca_startt (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct mca_header*)(void*) &pkt->body)->start_time;
#else
	return ntohl (((struct mca_header*)(void*) &pkt->body)->start_time);
#endif
}

static inline uint64_t
mca_stopt (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct mca_header*)(void*) &pkt->body)->stop_time;
#else
	return ntohl (((struct mca_header*)(void*) &pkt->body)->stop_time);
#endif
}

static inline uint32_t
mca_bin (fpga_pkt* pkt, uint16_t bin)
{
	if (is_header (pkt))
#ifdef KEEP_BYTEORDER
		return pkt->body[ bin*BIN_LEN + MCA_HDR_LEN ];
	#else
		return ntohl (pkt->body[ bin*BIN_LEN + MCA_HDR_LEN ]);
	#endif
	else
#ifdef KEEP_BYTEORDER
		return pkt->body[ bin*BIN_LEN ];
	#else
		return ntohl (pkt->body[ bin*BIN_LEN ]);
	#endif
}

static inline struct mca_flags*
mca_fl (fpga_pkt* pkt)
{
	struct mca_header* mh = (struct mca_header*)(void*) &pkt->body; 
	return (struct mca_flags*) &mh->flags;
}

static inline struct event_flags*
evt_fl (fpga_pkt* pkt)
{
	struct evt_header* eh = (struct evt_header*)(void*) &pkt->body;
	return (struct event_flags*) &eh->flags;
}

static inline struct tick_flags*
tick_fl (fpga_pkt* pkt)
{
	struct evt_header* eh = (struct evt_header*)(void*) &pkt->body;
	return (struct tick_flags*) &eh->flags;
}

static inline struct trace_flags*
trace_fl (fpga_pkt* pkt)
{
	struct trace_header* th = (struct trace_header*)(void*) &pkt->body;
	return (struct trace_flags*) &th->tr_flags;
}

static inline uint16_t
evt_toff (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct evt_header*)(void*) &pkt->body)->toff;
#else
	return ntohs (((struct evt_header*)(void*) &pkt->body)->toff);
#endif
}

static inline uint32_t
tick_period (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct tick_header*)(void*) &pkt->body)->period;
#else
	return ntohs (((struct tick_header*)(void*) &pkt->body)->period);
#endif
}

static inline uint64_t
tick_ts (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct tick_header*)(void*) &pkt->body)->ts;
#else
	return ntohs (((struct tick_header*)(void*) &pkt->body)->ts);
#endif
}

static inline uint8_t
tick_ovrfl (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct tick_header*)(void*) &pkt->body)->ovrfl;
#else
	return ntohs (((struct tick_header*)(void*) &pkt->body)->ovrfl);
#endif
}

static inline uint8_t
tick_err (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct tick_header*)(void*) &pkt->body)->err;
#else
	return ntohs (((struct tick_header*)(void*) &pkt->body)->err);
#endif
}

static inline uint8_t
tick_cfd (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct tick_header*)(void*) &pkt->body)->cfd;
#else
	return ntohs (((struct tick_header*)(void*) &pkt->body)->cfd);
#endif
}

static inline uint32_t
tick_lost (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct tick_header*)(void*) &pkt->body)->lost;
#else
	return ntohs (((struct tick_header*)(void*) &pkt->body)->lost);
#endif
}

static inline uint16_t
peak_ht (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct peak_header*)(void*) &pkt->body)->height;
#else
	return ntohs (((struct peak_header*)(void*) &pkt->body)->height);
#endif
}

static inline uint16_t
peak_riset (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct peak_header*)(void*) &pkt->body)->rise_time;
#else
	return ntohs (((struct peak_header*)(void*) &pkt->body)->rise_time);
#endif
}

static inline uint32_t
area_area (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct area_header*)(void*) &pkt->body)->area;
#else
	return ntohs (((struct area_header*)(void*) &pkt->body)->area);
#endif
}

static inline uint16_t
pulse_size (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct pulse_header*)(void*) &pkt->body)->size;
#else
	return ntohs (((struct pulse_header*)(void*) &pkt->body)->size);
#endif
}

static inline uint32_t
pulse_area (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct pulse_header*)(void*) &pkt->body)->pulse.area;
#else
	return ntohs (((struct pulse_header*)(void*) &pkt->body)->pulse.area);
#endif
}

static inline uint16_t
pulse_len (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct pulse_header*)(void*) &pkt->body)->pulse.length;
#else
	return ntohs (((struct pulse_header*)(void*) &pkt->body)->pulse.length);
#endif
}

static inline uint16_t
pulse_toff (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct pulse_header*)(void*) &pkt->body)->pulse.toffset;
#else
	return ntohs (((struct pulse_header*)(void*) &pkt->body)->pulse.toffset);
#endif
}

static inline uint16_t
trace_size (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct trace_header*)(void*) &pkt->body)->size;
#else
	return ntohs (((struct trace_header*)(void*) &pkt->body)->size);
#endif
}

static inline uint32_t
trace_area (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct trace_full_header*)(void*) &pkt->body)->pulse.area;
#else
	return ntohs (((struct trace_full_header*)(void*) &pkt->body)->pulse.area);
#endif
}

static inline uint16_t
trace_len (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct trace_full_header*)(void*) &pkt->body)->pulse.length;
#else
	return ntohs (((struct trace_full_header*)(void*) &pkt->body)->pulse.length);
#endif
}

static inline uint16_t
trace_toff (fpga_pkt* pkt)
{
#ifdef KEEP_BYTEORDER
	return ((struct trace_full_header*)(void*) &pkt->body)->pulse.toffset;
#else
	return ntohs (((struct trace_full_header*)(void*) &pkt->body)->pulse.toffset);
#endif
}

static void
pkt_pretty_print (fpga_pkt* pkt, FILE* stream)
{
	fprintf (stream, "Destination MAC:     %s\n",  dst_eth_ntoa (pkt));
	fprintf (stream, "Source MAC:          %s\n",  src_eth_ntoa (pkt));
	fprintf (stream, "Packet length:       %hu\n", pkt_len (pkt));
	fprintf (stream, "Frame sequence:      %hu\n", frame_seq (pkt));
	fprintf (stream, "Protocol sequence:   %hu\n", proto_seq (pkt));

	/* ----- MCA */
	if (is_mca (pkt))
	{
		fprintf (stream, "Stream type:         MCA\n");
		fprintf (stream, "Number of bins:      %u\n",   mca_num_bins (pkt));
		if (!is_header (pkt))
			return;
		fprintf (stream, "Size:                %hu\n",  mca_size (pkt));
		struct mca_flags* mf = mca_fl (pkt);
		fprintf (stream, "Flag Q:              %hhu\n", mf->Q);
		fprintf (stream, "Flag V:              %hhu\n", mf->V);
		fprintf (stream, "Flag T:              %hhu\n", mf->T);
		fprintf (stream, "Flag N:              %hhu\n", mf->N);
		fprintf (stream, "Flag C:              %hhu\n", mf->C);
		fprintf (stream, "Total number of bins:%u\n",   mca_num_allbins (pkt));
		fprintf (stream, "Lowest value:        %hu\n",  mca_lvalue (pkt));
		fprintf (stream, "Most frequent bin:   %hu\n",  mca_mfreq (pkt));
		fprintf (stream, "Total:               %lu\n",  mca_total (pkt));
		fprintf (stream, "Start time:          %lu\n",  mca_startt (pkt));
		fprintf (stream, "Stop time:           %lu\n",  mca_stopt (pkt));
		return;
	}
	if (!is_evt (pkt))
	{
		fprintf (stream, "Unknown stream type\n");
		return;
	}

	/* ----- Event */
	fprintf (stream, "Stream type:         Event\n");
	fprintf (stream, "Event size:          %hu\n", evt_size (pkt));
	fprintf (stream, "Time offset:         %hu\n", evt_toff (pkt));
	/* ---------- Tick event */
	if (is_tick (pkt))
	{
		struct tick_flags* tf = tick_fl (pkt);
		fprintf (stream, "Tick flag MF:        %hhu\n", tf->MF);
		fprintf (stream, "Tick flag EL:        %hhu\n", tf->EL);
		fprintf (stream, "Tick flag TL:        %hhu\n", tf->TL);
		fprintf (stream, "Tick flag T:         %hhu\n", tf->T);
		fprintf (stream, "Tick flag N:         %hhu\n", tf->N);
		fprintf (stream, "Period:              %u\n",   tick_period (pkt));
		fprintf (stream, "Timestamp:           %lu\n",  tick_ts (pkt));
		fprintf (stream, "Error ovrfl:         %hhu\n", tick_ovrfl (pkt));
		fprintf (stream, "Error err:           %hhu\n", tick_err (pkt));
		fprintf (stream, "Error cfd:           %hhu\n", tick_cfd (pkt));
		fprintf (stream, "Events lost:         %u\n",   tick_lost (pkt));
		fprintf (stream, "Type:                Tick\n");
		return;
	}
	/* ---------- Non-tick event */
	struct event_flags* ef = evt_fl (pkt);
	fprintf (stream, "Event flag PC:       %hhu\n", ef->PC);
	fprintf (stream, "Event flag O:        %hhu\n", ef->O);
	fprintf (stream, "Event flag CH:       %hhu\n", ef->CH);
	fprintf (stream, "Event flag TT:       %hhu\n", ef->TT);
	fprintf (stream, "Event flag HT:       %hhu\n", ef->HT);
	fprintf (stream, "Event flag PT:       %hhu\n", ef->PT);
	fprintf (stream, "Event flag T:        %hhu\n", ef->T);
	fprintf (stream, "Event flag N:        %hhu\n", ef->N);
	/* --------------- Peak */
	if (is_peak (pkt))
	{
		fprintf (stream, "Type:                Peak\n");
		fprintf (stream, "Height:              %hu\n", peak_ht (pkt));
		fprintf (stream, "Rise time:           %hu\n", peak_riset (pkt));
		return;
	}
	/* --------------- Area */
	if (is_area (pkt))
	{
		fprintf (stream, "Type:                Area\n");
		fprintf (stream, "Area:                %u\n", area_area (pkt));
		return;
	}
	/* --------------- Pulse */
	if (is_pulse (pkt))
	{
		fprintf (stream, "Type:                Pulse\n");
		fprintf (stream, "Size:                %hu\n", pulse_size (pkt));
		fprintf (stream, "Area:                %u\n",  pulse_area (pkt));
		fprintf (stream, "Length:              %hu\n", pulse_len (pkt));
		fprintf (stream, "Time offset:         %hu\n", pulse_toff (pkt));
		return;
	}
	if (!is_trace (pkt))
	{
		fprintf (stream, "Unknown event type\n");
		return;
	}
	/* --------------- Trace */
	fprintf (stream, "Type:                Trace\n");
	struct trace_flags* trf = trace_fl (pkt);
	fprintf (stream, "Trace flag MH:       %hhu\n", trf->MH);
	fprintf (stream, "Trace flag MP:       %hhu\n", trf->MP);
	fprintf (stream, "Trace flag STR:      %hhu\n", trf->STR);
	fprintf (stream, "Trace flag TT:       %hhu\n", trf->TT);
	fprintf (stream, "Trace flag TS:       %hhu\n", trf->TS);
	fprintf (stream, "Trace flag OFF:      %hhu\n", trf->OFF);
	fprintf (stream, "Trace size:          %hu\n", trace_size (pkt));
	/* -------------------- Average */
	if (is_trace_avg (pkt))
	{
		fprintf (stream, "Trace type:          Average\n");
		return;
	}
	fprintf (stream, "Area:                %u\n",  trace_area (pkt));
	fprintf (stream, "Length:              %hu\n", trace_len (pkt));
	fprintf (stream, "Time offset:         %hu\n", trace_toff (pkt));
	/* -------------------- Single */
	if (is_trace_sgl (pkt))
	{
		fprintf (stream, "Trace type:          Single\n");
		return;
	}
	/* -------------------- Dot product */
	if (is_trace_dp (pkt))
	{
		fprintf (stream, "Trace type:          Dot product\n");
		// fprintf (stream, "Dot product:         \n", );
		return;
	}
	/* -------------------- Dot product + trace */
	if (is_trace_dptr (pkt))
	{
		fprintf (stream, "Trace type:          Dot product with trace\n");
		// fprintf (stream, "Dot product:         \n", );
		return;
	}
	fprintf (stream, "Unknown trace type\n");
}

/* TO DO: check for contradicting fields, e.g. MCA's size contradicts last_bin */
/* Return codes */
#define FE_ETHTYPE 1 << 0 // ether type 
#define FE_ETHLEN  1 << 1 // frame length
#define FE_EVTTYPE 1 << 3 // event type 
#define FE_EVTSIZE 1 << 4 // event size for fixed size events
static int
is_valid (fpga_pkt* pkt)
{
	int rc = 0;
	/* Frame length should be a multiple of 8. */
	if (pkt_len (pkt) & 7 || pkt_len (pkt) > MAX_FPGA_FRAME_LEN)
		rc |= FE_ETHLEN;

	if (is_evt (pkt))
	{
		if (is_tick (pkt))
		{
			if (evt_size (pkt) != 3)
				rc |= FE_EVTSIZE;
		}
		else if (is_trace (pkt) || is_peak (pkt) || is_area (pkt))
		{
			if (evt_size (pkt) != 1)
				rc |= FE_EVTSIZE;
		}
		else if (!is_pulse (pkt))
			rc |= FE_EVTTYPE;
	}
	else if (!is_mca (pkt))
		rc |= FE_ETHTYPE;
	return rc;
}

static void
pkt_perror (FILE* stream, int err)
{
	if (err & FE_ETHTYPE)
		fprintf (stream, "Invalid ether type\n");
	if (err & FE_ETHLEN)
		fprintf (stream, "Invalid frame length\n");
	if (err & FE_EVTTYPE)
		fprintf (stream, "Invalid event type\n");
	if (err & FE_EVTSIZE)
		fprintf (stream, "Invalid event size\n");
}

/* ------------------------------------------------------------------------- */
/* --------------------------------- DEBUG --------------------------------- */
/* ------------------------------------------------------------------------- */

#ifdef FPGAPKT_DEBUG
#if __BYTE_ORDER == __LITTLE_ENDIAN
#  define ftoul(fl_ptr) ntohl ( *( (uint32_t*) (void*) fl_ptr ) )
#  define ftous(fl_ptr) ntohs ( *( (uint16_t*) (void*) fl_ptr ) )
#else
#  define ftoul(fl_ptr) ( *( (uint32_t*) (void*) fl_ptr ) )
#  define ftous(fl_ptr) ( *( (uint16_t*) (void*) fl_ptr ) )
#endif

static void
fpgapkt_self_test (void)
{
	assert (sizeof (fpga_pkt) == MAX_FPGA_FRAME_LEN);
	assert (offsetof (fpga_pkt, body) == FPGA_HDR_LEN);
	assert (sizeof (struct mca_header) == MCA_HDR_LEN);
	assert (sizeof (struct tick_header) == TICK_HDR_LEN);
	assert (sizeof (struct peak_header) == PEAK_HDR_LEN);
	assert (sizeof (struct peak) == PEAK_LEN);
	assert (sizeof (struct area_header) == AREA_HDR_LEN);
	assert (sizeof (struct pulse) == PLS_LEN);
	assert (sizeof (struct pulse_header) == PLS_HDR_LEN);
	assert (sizeof (struct trace_header) == TR_HDR_LEN);
	assert (sizeof (struct trace_full_header) == TR_FULL_HDR_LEN);
	assert (sizeof (struct dot_prod) == DP_LEN);
	assert (sizeof (struct mca_flags) == MCA_FL_LEN);
	assert (sizeof (struct event_flags) == EVT_FL_LEN);
	assert (sizeof (struct tick_flags) == EVT_FL_LEN);
	assert (sizeof (struct trace_flags) == TR_FL_LEN);

	struct mca_flags mf;
	memset (&mf, 0, MCA_FL_LEN);
	mf.Q = 0x0f;
	mf.V = 0x0f;
	mf.T = 0x0f;
	mf.N = 0x1f;
	mf.C = 0x07;
	assert ( ftoul (&mf) == MCA_FL_MASK );

	struct event_flags ef;
	memset (&ef, 0, EVT_FL_LEN);
	ef.PC = 0x0f;
	ef.O  = 0x01;
	ef.CH = 0x07;
	ef.TT = 0x03;
	ef.HT = 0x03;
	ef.PT = 0x03;
	ef.T  = 0x01;
	ef.N  = 0x01;
	assert ( ftous (&ef) == EVT_FL_MASK );

	struct tick_flags tf;
	memset (&tf, 0, EVT_FL_LEN);
	tf.MF = 0x01;
	tf.EL = 0x01;
	tf.TL = 0x01;
	tf.T  = 0x01;
	tf.N  = 0x01;
	assert ( ftous (&tf) == TICK_FL_MASK );

	struct trace_flags trf;
	memset (&trf, 0, TR_FL_LEN);
	trf.MH  = 0x01;
	trf.MP  = 0x01;
	trf.STR = 0x1f;
	trf.TT  = 0x03;
	trf.TS  = 0x03;
	trf.OFF = 0x0f;
	assert ( ftous (&trf) == TR_FL_MASK );
}

#endif /* FPGAPKT_DEBUG */

#endif
