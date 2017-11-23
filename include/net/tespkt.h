#ifndef __NET_TESPKT_H_INCLUDED__
#define __NET_TESPKT_H_INCLUDED__

#ifdef linux
#  include <byteswap.h>
#  define bswap16 bswap_16
#  define bswap32 bswap_32
#  define bswap64 bswap_64
#else
#  include <sys/endian.h>
#endif
/*

 * Ethernet header is always network-order (big-endian) but the byte order of
 * the payload can be changed.
 */
#define TES_BYTE_ORDER __LITTLE_ENDIAN 
#if __BYTE_ORDER == TES_BYTE_ORDER
#  define ftohs
#  define ftohl
#else
#  define ftohs bswap16
#  define ftohl bswap32
#endif

/* on FreeBSD sys/types is not included by net/ethernet, but is needed */
#ifndef linux
#  include <sys/types.h>
#endif
#include <net/ethernet.h>
#ifdef linux
/* ntoa, aton; on FreeBSD these are provided by net/ethernet */
#  include <netinet/ether.h>
#endif /* linux */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

/* ------------------------------------------------------------------------- */

typedef struct tespkt tespkt;

/* Destination and source MAC addresses */
static inline char*  tespkt_dst_eth_ntoa (tespkt* pkt);
static inline char*  tespkt_src_eth_ntoa (tespkt* pkt);
static inline struct ether_addr* tespkt_dst_eth_aton (tespkt* pkt);
static inline struct ether_addr* tespkt_src_eth_aton (tespkt* pkt);
/* Frame length (including ethernet header) */
static inline uint16_t tespkt_flen (tespkt* pkt);
/* Frame and protocol sequences */
static inline uint16_t tespkt_fseq (tespkt* pkt);
static inline uint16_t tespkt_pseq (tespkt* pkt);
/* True or false if frame is any header frame (protocol sequence == 0) */
static inline int tespkt_is_header (tespkt* pkt);
/* True or false if frame is any MCA or any event frame (the two are
 * complementary) */
static inline int tespkt_is_mca   (tespkt* pkt);
static inline int tespkt_is_evt   (tespkt* pkt);
/* True or false if frame is a tick, peak, area or pulse event */
static inline int tespkt_is_tick  (tespkt* pkt);
static inline int tespkt_is_peak  (tespkt* pkt);
static inline int tespkt_is_area  (tespkt* pkt);
static inline int tespkt_is_pulse (tespkt* pkt);
/* True or false if frame is any trace event */
static inline int tespkt_is_trace (tespkt* pkt);
/* True or false if frame is a single trace event */
static inline int tespkt_is_trace_sgl  (tespkt* pkt);
/* True or false if frame is a single, average, dot-product or
 * trace-dot-product trace event */
static inline int tespkt_is_trace_avg  (tespkt* pkt);
static inline int tespkt_is_trace_dp   (tespkt* pkt);
static inline int tespkt_is_trace_dptr (tespkt* pkt);
/* Event's size (valid for all events) */
static inline uint16_t tespkt_evt_size (tespkt* pkt);
/* Number of events in an event frame */
static inline uint16_t tespkt_evt_nums (tespkt* pkt);
/* Size of histogram (valid for MCA header frames) */
static inline uint16_t tespkt_mca_size (tespkt* pkt);
/* Number of bins in this frame (valid for all MCA frames) */
static inline uint16_t tespkt_mca_nbins (tespkt* pkt);
/* Number of bins in entire histogram (valid for MCA header frames) */
static inline uint16_t tespkt_mca_nbins_tot (tespkt* pkt);
/* Histogram's lowest value, most frequent bin, sum of all bins, start and stop
 * timestamp (valid for MCA header frames) */
static inline uint32_t tespkt_mca_lvalue (tespkt* pkt);
static inline uint16_t tespkt_mca_mfreq  (tespkt* pkt);
static inline uint64_t tespkt_mca_total  (tespkt* pkt);
static inline uint64_t tespkt_mca_startt (tespkt* pkt);
static inline uint64_t tespkt_mca_stopt  (tespkt* pkt);
/* Get bin number <bin> (starting at 0) of the current frame */
static inline uint32_t tespkt_mca_bin (tespkt* pkt, uint16_t bin);
/* MCA flags as a struct with separate fields for each register (valid for all
 * MCA frames) */
static inline struct tespkt_mca_flags*   tespkt_mca_fl  (tespkt* pkt);
/* Event (or tick) flags as a struct with separate fields for each register
 * (valid for all event frames) */
static inline struct tespkt_event_flags* tespkt_evt_fl  (tespkt* pkt);
static inline struct tespkt_tick_flags*  tespkt_tick_fl (tespkt* pkt);
/* Trace flags as a struct with separate fields for each register  (valid for
 * trace events) */
static inline struct tespkt_trace_flags* tespkt_trace_fl (tespkt* pkt);
/* Event's time (valid for all events) */
static inline uint16_t tespkt_evt_toff (tespkt* pkt);
/* Tick's period, timestamp, error registers and events lost (valid for tick
 * events) */
static inline uint32_t tespkt_tick_period (tespkt* pkt);
static inline uint64_t tespkt_tick_ts     (tespkt* pkt);
static inline uint8_t  tespkt_tick_ovrfl  (tespkt* pkt);
static inline uint8_t  tespkt_tick_err    (tespkt* pkt);
static inline uint8_t  tespkt_tick_cfd    (tespkt* pkt);
static inline uint32_t tespkt_tick_lost   (tespkt* pkt);
/* Peak's height and rise time (valid for peak events)  */
static inline uint16_t tespkt_peak_ht    (tespkt* pkt);
static inline uint16_t tespkt_peak_riset (tespkt* pkt);
/* Area's area (valid for area events) */
static inline uint32_t tespkt_area_area  (tespkt* pkt);
/* FIX: Pulse's size, area, length, time offset (valid for pulse events) */
static inline uint16_t tespkt_pulse_size (tespkt* pkt);
static inline uint32_t tespkt_pulse_area (tespkt* pkt);
static inline uint16_t tespkt_pulse_len  (tespkt* pkt);
static inline uint16_t tespkt_pulse_toff (tespkt* pkt);
/* FIX: Trace's size (valid for all trace events except average) */
static inline uint16_t tespkt_trace_size (tespkt* pkt);
/* FIX: Trace's area, length, time offset (valid for all trace events) */
static inline uint32_t tespkt_trace_area (tespkt* pkt);
static inline uint16_t tespkt_trace_len  (tespkt* pkt);
static inline uint16_t tespkt_trace_toff (tespkt* pkt);
/* Print info about packet */
static void pkt_pretty_print (tespkt* pkt, FILE* ostream, FILE* estream);
/* Check if packet is valid, returns 0 if all is ok, or one or more OR-ed flags */
static int  tespkt_is_valid (tespkt* pkt);
/* Print info about each of the flags present in the return value of tespkt_is_valid */
static void tespkt_perror (FILE* stream, int err);
/* As above, but print to a char* buffer, rather than a FILE* one */
static void tespkt_serror (char* stream, int err);

#ifdef TESPKT_DEBUG
static void tespkt_self_test (void);
#endif

/*
 * You can copy a flag integer into one of these structures to read off the
 * separate registers. Flags are always sent as big-endian.
 */
struct tespkt_mca_flags
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

struct tespkt_event_flags
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

struct tespkt_tick_flags
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

struct tespkt_trace_flags
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

#define TES_HDR_LEN  24 /* includes the 16 byte ethernet header */
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
#define MAX_TES_FRAME_LEN  1496

#define ETHERTYPE_F_EVENT    0x88B5
#define ETHERTYPE_F_MCA      0x88B6

/*
 * Event types are sent as separate bytes, i.e. always appear big-endian.
 * Redefine them for little-endian hosts, instead of using ntohl/s, since we
 * never return those as 16-bit integers (only used in tespkt_is_* helpers).
 */
#if __BYTE_ORDER == __LITTLE_ENDIAN
#  define EVT_TYPE_MASK     0x0e03 /* all relevant bits of etype */
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
#  define EVT_TYPE_MASK     0x030e /* all relevant bits of etype */
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

struct tespkt_mca_hdr
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
struct tespkt_evt_hdr
{
	uint32_t : 32;
	uint16_t flags;
	uint16_t toff;
};

struct tespkt_tick_hdr
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

struct tespkt_peak_hdr
{
	uint16_t height;
	uint16_t rise_time;
	uint16_t flags;
	uint16_t toff;
};

struct tespkt_peak
{
	uint16_t height;
	uint16_t rise_time;
	uint16_t minimum;
	uint16_t toff;
};

struct tespkt_area_hdr
{
	uint32_t area;
	uint16_t flags;
	uint16_t toff;
};

struct tespkt_pulse
{
	uint32_t area;
	uint16_t length;
	uint16_t toffset;
};

struct tespkt_pulse_hdr
{
	uint16_t size;
	uint16_t : 16; /* reserved */
	uint16_t flags;
	uint16_t toff;
	struct   tespkt_pulse pulse;
};

struct tespkt_trace_hdr
{
	uint16_t size;
	uint16_t tr_flags;
	uint16_t flags;
	uint16_t toff;
};

struct tespkt_trace_full_hdr
{
	struct tespkt_trace_hdr trace;
	struct tespkt_pulse pulse;
};

struct tespkt_dot_prod
{
	uint16_t : 16; /* reserved */
	uint64_t dot_prod : 48;
} __attribute__ ((__packed__));

struct tespkt
{
	struct
	{
		struct ether_header eth_hdr; /* packed, 14 bytes */
		uint16_t length;	     /* length of packet */
	};
	struct
	{
		uint16_t fseq;
		uint16_t pseq;
		uint16_t esize; /* undefined for MCA frames */
		uint16_t etype; /* undefined for MCA frames */
	} tes_hdr;
	char body[MAX_TES_FRAME_LEN - TES_HDR_LEN];
};

/* ------------------------------------------------------------------------- */

static inline int
tespkt_is_header (tespkt* pkt)
{ /* Byte order is irrelevant */
	return ( pkt->tes_hdr.pseq == 0 );
}

/* Ethernet type is always big-endian */
static inline int
tespkt_is_mca (tespkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ntohs (ETHERTYPE_F_MCA) );
}

static inline int
tespkt_is_evt (tespkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ntohs (ETHERTYPE_F_EVENT) );
}

static inline int
tespkt_is_tick (tespkt* pkt)
{
	return ( tespkt_is_evt (pkt) && pkt->tes_hdr.etype & EVT_TICK_TYPE );
}

static inline int
tespkt_is_peak (tespkt* pkt)
{
	return ( tespkt_is_evt (pkt) &&
		(pkt->tes_hdr.etype & EVT_PKT_TYPE_MASK)
			== EVT_PEAK_TYPE );
}

static inline int
tespkt_is_pulse (tespkt* pkt)
{
	return ( tespkt_is_evt (pkt) &&
		(pkt->tes_hdr.etype & EVT_PKT_TYPE_MASK)
			== EVT_PLS_TYPE );
}

static inline int
tespkt_is_area (tespkt* pkt)
{
	return ( tespkt_is_evt (pkt) &&
		(pkt->tes_hdr.etype & EVT_PKT_TYPE_MASK)
			== EVT_AREA_TYPE );
}

static inline int
tespkt_is_trace (tespkt* pkt)
{
	return ( tespkt_is_evt (pkt) &&
		(pkt->tes_hdr.etype & EVT_PKT_TYPE_MASK)
			== EVT_TR_TYPE );
}

static inline int
tespkt_is_trace_sgl (tespkt* pkt)
{
	return ( tespkt_is_evt (pkt) &&
		(pkt->tes_hdr.etype & EVT_TYPE_MASK)
			== EVT_TR_SGL_TYPE );
}

static inline int
tespkt_is_trace_avg (tespkt* pkt)
{
	return ( tespkt_is_evt (pkt) &&
		(pkt->tes_hdr.etype & EVT_TYPE_MASK)
			== EVT_TR_AVG_TYPE );
}

static inline int
tespkt_is_trace_dp (tespkt* pkt)
{
	return ( tespkt_is_evt (pkt) &&
		(pkt->tes_hdr.etype & EVT_TYPE_MASK)
			== EVT_TR_DP_TYPE );
}

static inline int
tespkt_is_trace_dptr (tespkt* pkt)
{
	return ( tespkt_is_evt (pkt) &&
		(pkt->tes_hdr.etype & EVT_TYPE_MASK)
			== EVT_TR_DPTR_TYPE );
}

static inline char*
tespkt_dst_eth_ntoa (tespkt* pkt)
{
	return ether_ntoa ((struct ether_addr*) pkt->eth_hdr.ether_dhost);
}

static inline char*
tespkt_src_eth_ntoa (tespkt* pkt)
{
	return ether_ntoa ((struct ether_addr*) pkt->eth_hdr.ether_shost);
}

static inline struct ether_addr*
tespkt_dst_eth_aton (tespkt* pkt)
{
	return ((struct ether_addr*) pkt->eth_hdr.ether_dhost);
}

static inline struct ether_addr*
tespkt_src_eth_aton (tespkt* pkt)
{
	return ((struct ether_addr*) pkt->eth_hdr.ether_shost);
}

static inline uint16_t
tespkt_flen (tespkt* pkt)
{
	return ftohs (pkt->length);
}

static inline uint16_t
tespkt_fseq (tespkt* pkt)
{
	return ftohs (pkt->tes_hdr.fseq);
}

static inline uint16_t
tespkt_pseq (tespkt* pkt)
{
	return ftohs (pkt->tes_hdr.pseq);
}

static inline uint16_t
tespkt_evt_size (tespkt* pkt)
{
	return ftohs (pkt->tes_hdr.esize);
}

static inline uint16_t
tespkt_evt_nums (tespkt* pkt)
{
	return ( ( tespkt_flen (pkt) - TES_HDR_LEN )
			/ ( tespkt_evt_size (pkt) << 3 ) );
}

static inline uint16_t
tespkt_mca_size (tespkt* pkt)
{
	return ftohl (((struct tespkt_mca_hdr*)(void*) &pkt->body)->size);
}

static inline uint16_t
tespkt_mca_nbins (tespkt* pkt)
{
	if (tespkt_is_header (pkt))
		return ( (tespkt_flen (pkt) - TES_HDR_LEN - MCA_HDR_LEN )
			/ BIN_LEN );
	else
		return ( (tespkt_flen (pkt) - TES_HDR_LEN) / BIN_LEN );
}

static inline uint16_t
tespkt_mca_nbins_tot (tespkt* pkt)
{
	return ftohl (((struct tespkt_mca_hdr*)(void*) &pkt->body)->last_bin + 1);
}

static inline uint32_t
tespkt_mca_lvalue (tespkt* pkt)
{
	return ftohl (((struct tespkt_mca_hdr*)(void*) &pkt->body)->lowest_value);
}

static inline uint16_t
tespkt_mca_mfreq (tespkt* pkt)
{
	return ftohl (((struct tespkt_mca_hdr*)(void*) &pkt->body)->most_frequent);
}

static inline uint64_t
tespkt_mca_total (tespkt* pkt)
{
	return ftohl (((struct tespkt_mca_hdr*)(void*) &pkt->body)->total);
}

static inline uint64_t
tespkt_mca_startt (tespkt* pkt)
{
	return ftohl (((struct tespkt_mca_hdr*)(void*) &pkt->body)->start_time);
}

static inline uint64_t
tespkt_mca_stopt (tespkt* pkt)
{
	return ftohl (((struct tespkt_mca_hdr*)(void*) &pkt->body)->stop_time);
}

static inline uint32_t
tespkt_mca_bin (tespkt* pkt, uint16_t bin)
{
	if (tespkt_is_header (pkt))
		return ftohl (pkt->body[ bin*BIN_LEN + MCA_HDR_LEN ]);
	else
		return ftohl (pkt->body[ bin*BIN_LEN ]);
}

static inline struct tespkt_mca_flags*
tespkt_mca_fl (tespkt* pkt)
{
	struct tespkt_mca_hdr* mh = (struct tespkt_mca_hdr*)(void*) &pkt->body; 
	return (struct tespkt_mca_flags*) &mh->flags;
}

static inline struct tespkt_event_flags*
tespkt_evt_fl (tespkt* pkt)
{
	struct tespkt_evt_hdr* eh = (struct tespkt_evt_hdr*)(void*) &pkt->body;
	return (struct tespkt_event_flags*) &eh->flags;
}

static inline struct tespkt_tick_flags*
tespkt_tick_fl (tespkt* pkt)
{
	struct tespkt_evt_hdr* eh = (struct tespkt_evt_hdr*)(void*) &pkt->body;
	return (struct tespkt_tick_flags*) &eh->flags;
}

static inline struct tespkt_trace_flags*
tespkt_trace_fl (tespkt* pkt)
{
	struct tespkt_trace_hdr* th = (struct tespkt_trace_hdr*)(void*) &pkt->body;
	return (struct tespkt_trace_flags*) &th->tr_flags;
}

static inline uint16_t
tespkt_evt_toff (tespkt* pkt)
{
	return ftohs (((struct tespkt_evt_hdr*)(void*) &pkt->body)->toff);
}

static inline uint32_t
tespkt_tick_period (tespkt* pkt)
{
	return ftohs (((struct tespkt_tick_hdr*)(void*) &pkt->body)->period);
}

static inline uint64_t
tespkt_tick_ts (tespkt* pkt)
{
	return ftohs (((struct tespkt_tick_hdr*)(void*) &pkt->body)->ts);
}

static inline uint8_t
tespkt_tick_ovrfl (tespkt* pkt)
{
	return ftohs (((struct tespkt_tick_hdr*)(void*) &pkt->body)->ovrfl);
}

static inline uint8_t
tespkt_tick_err (tespkt* pkt)
{
	return ftohs (((struct tespkt_tick_hdr*)(void*) &pkt->body)->err);
}

static inline uint8_t
tespkt_tick_cfd (tespkt* pkt)
{
	return ftohs (((struct tespkt_tick_hdr*)(void*) &pkt->body)->cfd);
}

static inline uint32_t
tespkt_tick_lost (tespkt* pkt)
{
	return ftohs (((struct tespkt_tick_hdr*)(void*) &pkt->body)->lost);
}

static inline uint16_t
tespkt_peak_ht (tespkt* pkt)
{
	return ftohs (((struct tespkt_peak_hdr*)(void*) &pkt->body)->height);
}

static inline uint16_t
tespkt_peak_riset (tespkt* pkt)
{
	return ftohs (((struct tespkt_peak_hdr*)(void*) &pkt->body)->rise_time);
}

static inline uint32_t
tespkt_area_area (tespkt* pkt)
{
	return ftohs (((struct tespkt_area_hdr*)(void*) &pkt->body)->area);
}

static inline uint16_t
tespkt_pulse_size (tespkt* pkt)
{
	return ftohs (((struct tespkt_pulse_hdr*)(void*) &pkt->body)->size);
}

static inline uint32_t
tespkt_pulse_area (tespkt* pkt)
{
	return ftohs (((struct tespkt_pulse_hdr*)(void*) &pkt->body)->pulse.area);
}

static inline uint16_t
tespkt_pulse_len (tespkt* pkt)
{
	return ftohs (((struct tespkt_pulse_hdr*)(void*) &pkt->body)->pulse.length);
}

static inline uint16_t
tespkt_pulse_toff (tespkt* pkt)
{
	return ftohs (((struct tespkt_pulse_hdr*)(void*) &pkt->body)->pulse.toffset);
}

static inline uint16_t
tespkt_trace_size (tespkt* pkt)
{
	return ftohs (((struct tespkt_trace_hdr*)(void*) &pkt->body)->size);
}

static inline uint32_t
tespkt_trace_area (tespkt* pkt)
{
	return ftohs (((struct tespkt_trace_full_hdr*)(void*) &pkt->body)->pulse.area);
}

static inline uint16_t
tespkt_trace_len (tespkt* pkt)
{
	return ftohs (((struct tespkt_trace_full_hdr*)(void*) &pkt->body)->pulse.length);
}

static inline uint16_t
tespkt_trace_toff (tespkt* pkt)
{
	return ftohs (((struct tespkt_trace_full_hdr*)(void*) &pkt->body)->pulse.toffset);
}

static void
pkt_pretty_print (tespkt* pkt, FILE* ostream, FILE* estream)
{
	if (estream == NULL)
		estream = ostream;

	fprintf (ostream, "Destination MAC:     %s\n",  tespkt_dst_eth_ntoa (pkt));
	fprintf (ostream, "Source MAC:          %s\n",  tespkt_src_eth_ntoa (pkt));
	fprintf (ostream, "Packet length:       %hu\n", tespkt_flen (pkt));
	fprintf (ostream, "Frame sequence:      %hu\n", tespkt_fseq (pkt));
	fprintf (ostream, "Protocol sequence:   %hu\n", tespkt_pseq (pkt));

	/* ----- MCA */
	if (tespkt_is_mca (pkt))
	{
		fprintf (ostream, "Stream type:         MCA\n");
		fprintf (ostream, "Number of bins:      %u\n",   tespkt_mca_nbins (pkt));
		if (!tespkt_is_header (pkt))
			return;
		fprintf (ostream, "Size:                %hu\n",  tespkt_mca_size (pkt));
		struct tespkt_mca_flags* mf = tespkt_mca_fl (pkt);
		fprintf (ostream, "Flag Q:              %hhu\n", mf->Q);
		fprintf (ostream, "Flag V:              %hhu\n", mf->V);
		fprintf (ostream, "Flag T:              %hhu\n", mf->T);
		fprintf (ostream, "Flag N:              %hhu\n", mf->N);
		fprintf (ostream, "Flag C:              %hhu\n", mf->C);
		fprintf (ostream, "Total number of bins:%u\n",   tespkt_mca_nbins_tot (pkt));
		fprintf (ostream, "Lowest value:        %hu\n",  tespkt_mca_lvalue (pkt));
		fprintf (ostream, "Most frequent bin:   %hu\n",  tespkt_mca_mfreq (pkt));
		fprintf (ostream, "Total:               %lu\n",  tespkt_mca_total (pkt));
		fprintf (ostream, "Start time:          %lu\n",  tespkt_mca_startt (pkt));
		fprintf (ostream, "Stop time:           %lu\n",  tespkt_mca_stopt (pkt));
		return;
	}
	if (!tespkt_is_evt (pkt))
	{
		fprintf (estream, "Unknown stream type\n");
		return;
	}

	/* ----- Event */
	fprintf (ostream, "Stream type:         Event\n");
	fprintf (ostream, "Event size:          %hu\n", tespkt_evt_size (pkt));
	fprintf (ostream, "Time offset:         %hu\n", tespkt_evt_toff (pkt));
	/* ---------- Tick event */
	if (tespkt_is_tick (pkt))
	{
		struct tespkt_tick_flags* tf = tespkt_tick_fl (pkt);
		fprintf (ostream, "Tick flag MF:        %hhu\n", tf->MF);
		fprintf (ostream, "Tick flag EL:        %hhu\n", tf->EL);
		fprintf (ostream, "Tick flag TL:        %hhu\n", tf->TL);
		fprintf (ostream, "Tick flag T:         %hhu\n", tf->T);
		fprintf (ostream, "Tick flag N:         %hhu\n", tf->N);
		fprintf (ostream, "Period:              %u\n",   tespkt_tick_period (pkt));
		fprintf (ostream, "Timestamp:           %lu\n",  tespkt_tick_ts (pkt));
		fprintf (ostream, "Error ovrfl:         %hhu\n", tespkt_tick_ovrfl (pkt));
		fprintf (ostream, "Error err:           %hhu\n", tespkt_tick_err (pkt));
		fprintf (ostream, "Error cfd:           %hhu\n", tespkt_tick_cfd (pkt));
		fprintf (ostream, "Events lost:         %u\n",   tespkt_tick_lost (pkt));
		fprintf (ostream, "Type:                Tick\n");
		return;
	}
	/* ---------- Non-tick event */
	struct tespkt_event_flags* ef = tespkt_evt_fl (pkt);
	fprintf (ostream, "Event flag PC:       %hhu\n", ef->PC);
	fprintf (ostream, "Event flag O:        %hhu\n", ef->O);
	fprintf (ostream, "Event flag CH:       %hhu\n", ef->CH);
	fprintf (ostream, "Event flag TT:       %hhu\n", ef->TT);
	fprintf (ostream, "Event flag HT:       %hhu\n", ef->HT);
	fprintf (ostream, "Event flag PT:       %hhu\n", ef->PT);
	fprintf (ostream, "Event flag T:        %hhu\n", ef->T);
	fprintf (ostream, "Event flag N:        %hhu\n", ef->N);
	/* --------------- Peak */
	if (tespkt_is_peak (pkt))
	{
		fprintf (ostream, "Type:                Peak\n");
		fprintf (ostream, "Height:              %hu\n", tespkt_peak_ht (pkt));
		fprintf (ostream, "Rise time:           %hu\n", tespkt_peak_riset (pkt));
		return;
	}
	/* --------------- Area */
	if (tespkt_is_area (pkt))
	{
		fprintf (ostream, "Type:                Area\n");
		fprintf (ostream, "Area:                %u\n", tespkt_area_area (pkt));
		return;
	}
	/* --------------- Pulse */
	if (tespkt_is_pulse (pkt))
	{
		fprintf (ostream, "Type:                Pulse\n");
		fprintf (ostream, "Size:                %hu\n", tespkt_pulse_size (pkt));
		fprintf (ostream, "Area:                %u\n",  tespkt_pulse_area (pkt));
		fprintf (ostream, "Length:              %hu\n", tespkt_pulse_len (pkt));
		fprintf (ostream, "Time offset:         %hu\n", tespkt_pulse_toff (pkt));
		return;
	}
	if (!tespkt_is_trace (pkt))
	{
		fprintf (estream, "Unknown event type\n");
		return;
	}
	/* --------------- Trace */
	fprintf (ostream, "Type:                Trace\n");
	struct tespkt_trace_flags* trf = tespkt_trace_fl (pkt);
	fprintf (ostream, "Trace flag MH:       %hhu\n", trf->MH);
	fprintf (ostream, "Trace flag MP:       %hhu\n", trf->MP);
	fprintf (ostream, "Trace flag STR:      %hhu\n", trf->STR);
	fprintf (ostream, "Trace flag TT:       %hhu\n", trf->TT);
	fprintf (ostream, "Trace flag TS:       %hhu\n", trf->TS);
	fprintf (ostream, "Trace flag OFF:      %hhu\n", trf->OFF);
	fprintf (ostream, "Trace size:          %hu\n", tespkt_trace_size (pkt));
	/* -------------------- Average */
	if (tespkt_is_trace_avg (pkt))
	{
		fprintf (ostream, "Trace type:          Average\n");
		return;
	}
	fprintf (ostream, "Area:                %u\n",  tespkt_trace_area (pkt));
	fprintf (ostream, "Length:              %hu\n", tespkt_trace_len (pkt));
	fprintf (ostream, "Time offset:         %hu\n", tespkt_trace_toff (pkt));
	/* -------------------- Single */
	if (tespkt_is_trace_sgl (pkt))
	{
		fprintf (ostream, "Trace type:          Single\n");
		return;
	}
	/* -------------------- Dot product */
	if (tespkt_is_trace_dp (pkt))
	{
		fprintf (ostream, "Trace type:          Dot product\n");
		// fprintf (ostream, "Dot product:         \n", );
		return;
	}
	/* -------------------- Dot product + trace */
	if (tespkt_is_trace_dptr (pkt))
	{
		fprintf (ostream, "Trace type:          Dot product with trace\n");
		// fprintf (ostream, "Dot product:         \n", );
		return;
	}
	fprintf (estream, "Unknown trace type\n");
}

/* Return codes */
#define TES_EETHTYPE    1 // ether type 
#define TES_EETHLEN     2 // frame length
#define TES_EEVTTYPE    4 // event type 
#define TES_EEVTSIZE    8 // event size for fixed size events
#define TES_ETRSIZE    16 // event size for fixed size events
#define TES_EMCASIZE   32 // mismatch: size vs last bin
#define TES_EMCABINS   64 // mismatch: most frequent vs last bin
#define TES_EMCACHKSM 128 // mismatch: lowest value * no. bins vs sum total

#define TES_EETHTYPE_S  "Invalid ether type"
#define TES_EETHLEN_S   "Invalid frame length"
#define TES_EEVTTYPE_S  "Invalid event type"
#define TES_EEVTSIZE_S  "Invalid event size"
#define TES_ETRSIZE_S   "Invalid trace size"
#define TES_EMCASIZE_S  "Invalid histogram size"
#define TES_EMCABINS_S  "Invalid bin number in histogram"
#define TES_EMCACHKSM_S "Invalid sum total for histogram"
#define TES_EMAXLEN 64  // maximum length of error string

static int
tespkt_is_valid (tespkt* pkt)
{
	int rc = 0;

	uint16_t flen = tespkt_flen (pkt);

	/* Frame length should be a multiple of 8 */
	if (flen & 7 || flen > MAX_TES_FRAME_LEN)
		rc |= TES_EETHLEN;
	/* and it should be more than the header length. */
	if (flen <= TES_HDR_LEN)
		rc |= TES_EETHLEN;

	if (tespkt_is_evt (pkt))
	{
		uint16_t esize = tespkt_evt_size (pkt);

		/* Event size should not be 0. */
		if (esize == 0)
			rc |= TES_EEVTSIZE;

		/* Payload length should be a multiple of event size * 8. */
		if ( (flen - TES_HDR_LEN) % (esize << 3) != 0 )
			rc |= TES_EETHLEN;

		/* Check event type as well as size for types with a fixed
		 * size. */
		if (tespkt_is_tick (pkt))
		{
			if (esize != 3)
				rc |= TES_EEVTSIZE;
		}
		else if (tespkt_is_peak (pkt) || tespkt_is_area (pkt))
		{
			if (esize != 1)
				rc |= TES_EEVTSIZE;
		}
		else if (tespkt_is_trace (pkt))
		{
			/* Trace size should not be 0. */
			if (tespkt_is_header (pkt) &&
				tespkt_trace_size (pkt) == 0)
				rc |= TES_ETRSIZE;

			if ( ( ! tespkt_is_trace_dp (pkt) )
					&& esize != 1 )
				rc |= TES_EEVTSIZE;
		}
		else if (!tespkt_is_pulse (pkt))
			rc |= TES_EEVTTYPE;
	}
	else if (tespkt_is_mca (pkt))
	{
		uint16_t nbins_tot = tespkt_mca_nbins_tot (pkt);
		/* MCA size should correspond to last bin. */
		if (tespkt_mca_size (pkt) !=
			(nbins_tot * BIN_LEN) + MCA_HDR_LEN)
			rc |= TES_EMCASIZE;

		/* Most frequent bin cannot be greater than last bin. */
		if (tespkt_mca_mfreq (pkt) >= nbins_tot)
			rc |= TES_EMCABINS;

		/* Lowest value * no. bins cannot be greater than sum total. */
		if ( (tespkt_mca_lvalue (pkt) * nbins_tot) >
				tespkt_mca_total (pkt))
			rc |= TES_EMCACHKSM;

		/* TO DO: can the timestamps overflow, i.e. can stop time < start time? */
	}
	else
		rc |= TES_EETHTYPE;

	return rc;
}

static void
tespkt_perror (FILE* stream, int err)
{
	if (err & TES_EETHTYPE)
		fprintf (stream, "%s\n", TES_EETHTYPE_S);
	if (err & TES_EETHLEN)
		fprintf (stream, "%s\n", TES_EETHLEN_S);
	if (err & TES_EEVTTYPE)
		fprintf (stream, "%s\n", TES_EEVTTYPE_S);
	if (err & TES_EEVTSIZE)
		fprintf (stream, "%s\n", TES_EEVTSIZE_S);
	if (err & TES_ETRSIZE)
		fprintf (stream, "%s\n", TES_ETRSIZE_S);
	if (err & TES_EMCASIZE)
		fprintf (stream, "%s\n", TES_EMCASIZE_S);
	if (err & TES_EMCABINS)
		fprintf (stream, "%s\n", TES_EMCABINS_S);
	if (err & TES_EMCACHKSM)
		fprintf (stream, "%s\n", TES_EMCACHKSM_S);
}

static void
tespkt_serror (char* buf, int err)
{
	if (err & TES_EETHTYPE)
		snprintf (buf, TES_EMAXLEN, TES_EETHTYPE_S);
	if (err & TES_EETHLEN)
		snprintf (buf, TES_EMAXLEN, TES_EETHLEN_S);
	if (err & TES_EEVTTYPE)
		snprintf (buf, TES_EMAXLEN, TES_EEVTTYPE_S);
	if (err & TES_EEVTSIZE)
		snprintf (buf, TES_EMAXLEN, TES_EEVTSIZE_S);
	if (err & TES_ETRSIZE)
		snprintf (buf, TES_EMAXLEN, TES_ETRSIZE_S);
	if (err & TES_EMCASIZE)
		snprintf (buf, TES_EMAXLEN, TES_EMCASIZE_S);
	if (err & TES_EMCABINS)
		snprintf (buf, TES_EMAXLEN, TES_EMCABINS_S);
	if (err & TES_EMCACHKSM)
		snprintf (buf, TES_EMAXLEN, TES_EMCACHKSM_S);
}

/* ------------------------------------------------------------------------- */
/* --------------------------------- DEBUG --------------------------------- */
/* ------------------------------------------------------------------------- */

#ifdef TESPKT_DEBUG
#include <string.h>
#include <assert.h>

#define MCA_FL_MASK       0x000fffff
#define EVT_FL_MASK       0xffff
#define TICK_FL_MASK      0x0703
#define TR_FL_MASK        0x7fff

/* Flags are always sent as big-endian. */
#define flagtoul(fl_ptr) ntohl ( *( (uint32_t*) (void*) fl_ptr ) )
#define flagtous(fl_ptr) ntohs ( *( (uint16_t*) (void*) fl_ptr ) )

static void
tespkt_self_test (void)
{
	assert (sizeof (tespkt) == MAX_TES_FRAME_LEN);
	assert (offsetof (tespkt, body) == TES_HDR_LEN);
	assert (sizeof (struct tespkt_mca_hdr) == MCA_HDR_LEN);
	assert (sizeof (struct tespkt_tick_hdr) == TICK_HDR_LEN);
	assert (sizeof (struct tespkt_peak_hdr) == PEAK_HDR_LEN);
	assert (sizeof (struct tespkt_peak) == PEAK_LEN);
	assert (sizeof (struct tespkt_area_hdr) == AREA_HDR_LEN);
	assert (sizeof (struct tespkt_pulse) == PLS_LEN);
	assert (sizeof (struct tespkt_pulse_hdr) == PLS_HDR_LEN);
	assert (sizeof (struct tespkt_trace_hdr) == TR_HDR_LEN);
	assert (sizeof (struct tespkt_trace_full_hdr) == TR_FULL_HDR_LEN);
	assert (sizeof (struct tespkt_dot_prod) == DP_LEN);
	assert (sizeof (struct tespkt_mca_flags) == MCA_FL_LEN);
	assert (sizeof (struct tespkt_event_flags) == EVT_FL_LEN);
	assert (sizeof (struct tespkt_tick_flags) == EVT_FL_LEN);
	assert (sizeof (struct tespkt_trace_flags) == TR_FL_LEN);

	struct tespkt_mca_flags mf;
	memset (&mf, 0, MCA_FL_LEN);
	mf.Q = 0x0f;
	mf.V = 0x0f;
	mf.T = 0x0f;
	mf.N = 0x1f;
	mf.C = 0x07;
	assert ( flagtoul (&mf) == MCA_FL_MASK );

	struct tespkt_event_flags ef;
	memset (&ef, 0, EVT_FL_LEN);
	ef.PC = 0x0f;
	ef.O  = 0x01;
	ef.CH = 0x07;
	ef.TT = 0x03;
	ef.HT = 0x03;
	ef.PT = 0x03;
	ef.T  = 0x01;
	ef.N  = 0x01;
	assert ( flagtous (&ef) == EVT_FL_MASK );

	struct tespkt_tick_flags tf;
	memset (&tf, 0, EVT_FL_LEN);
	tf.MF = 0x01;
	tf.EL = 0x01;
	tf.TL = 0x01;
	tf.T  = 0x01;
	tf.N  = 0x01;
	assert ( flagtous (&tf) == TICK_FL_MASK );

	struct tespkt_trace_flags trf;
	memset (&trf, 0, TR_FL_LEN);
	trf.MH  = 0x01;
	trf.MP  = 0x01;
	trf.STR = 0x1f;
	trf.TT  = 0x03;
	trf.TS  = 0x03;
	trf.OFF = 0x0f;
	assert ( flagtous (&trf) == TR_FL_MASK );
}

#endif /* TESPKT_DEBUG */

#endif
