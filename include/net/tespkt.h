/*
 * Helpers for inspecting ethernet packets of the TES protocol.
 * 
 * One can either use struct tespkt directly and access its fields
 * (note that byte order needs to be taken into account then), or
 * use the helper functions below which take a pointer to it and
 * change the byte order where necessary. Type tespkt is aliased to
 * struct tespkt.
 *
 * TO DO:
 *  - add debugging checks to inline macros (ensure they are called on
 *    the correct packet type and that pointer arithmetic does not go
 *    beyond packet length.
 */

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
 * Ethernet header is always network-order (big-endian) but the byte
 * order of the payload can be changed.
 */
#define TES_BYTE_ORDER __LITTLE_ENDIAN 
#if __BYTE_ORDER == TES_BYTE_ORDER
#  define ftohs
#  define ftohl
#else
#  define ftohs bswap16
#  define ftohl bswap32
#endif

/* on FreeBSD sys/types is not included by net/ethernet but is
 * needed */
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

#ifdef TESPKT_DEBUG
#include <string.h>
#include <assert.h>
#endif

#define TES_MCASIZE_BUG /* overflow bug, last_bin is too large */
#ifndef TES_MCASIZE_BUG
#  define TES_HIST_MAXSIZE 65528U // highest 16-bit number multiple of 8
#else
#  define TES_HIST_MAXSIZE 65576U
#endif

#define TES_AVGTR_MAXSIZE 65528U // highest 16-bit number multiple of 8

typedef struct tespkt tespkt;

/* -------------------------------------------------------------- */

/*
 * Get destination and source MAC addresses.
 */
static inline char*  tespkt_dst_eth_ntoa (tespkt* pkt);
static inline char*  tespkt_src_eth_ntoa (tespkt* pkt);
static inline struct ether_addr* tespkt_dst_eth_aton (tespkt* pkt);
static inline struct ether_addr* tespkt_src_eth_aton (tespkt* pkt);

/*
 * Get frame length (including ethernet header).
 */
static inline uint16_t tespkt_flen (tespkt* pkt);

/*
 * Get frame and protocol sequences.
 */
static inline uint16_t tespkt_fseq (tespkt* pkt);
static inline uint16_t tespkt_pseq (tespkt* pkt);

/*
 * True or false if frame is a header frame (protocol sequence == 0).
 */
static inline int tespkt_is_header (tespkt* pkt);

/*
 * True or false if frame is any MCA or any event frame (the two are
 * complementary).
 */
static inline int tespkt_is_mca   (tespkt* pkt);
static inline int tespkt_is_event   (tespkt* pkt);

/*
 * True or false if frame is a tick, peak, area or pulse event.
 */
static inline int tespkt_is_tick  (tespkt* pkt);
static inline int tespkt_is_peak  (tespkt* pkt);
static inline int tespkt_is_area  (tespkt* pkt);
static inline int tespkt_is_pulse (tespkt* pkt);

/*
 * True or false if frame is any, single, average, dot-product or
 * trace-dot-product trace event.
 */
static inline int tespkt_is_trace (tespkt* pkt);
static inline int tespkt_is_trace_long (tespkt* pkt);
static inline int tespkt_is_trace_sgl  (tespkt* pkt);
static inline int tespkt_is_trace_avg  (tespkt* pkt);
static inline int tespkt_is_trace_dp   (tespkt* pkt);
static inline int tespkt_is_trace_dptr (tespkt* pkt);

/* ----------- Call the following only on MCA frames. ---------- */
/*
 * Get number of bins in this frame.
 */
static inline uint16_t tespkt_mca_nbins (tespkt* pkt);

/*
 * Get number of bins in entire histogram (only header frames).
 */
static inline uint16_t tespkt_mca_nbins_tot (tespkt* pkt);

/*
 * Get size of histogram (only header frames).
 */
#ifndef TES_MCASIZE_BUG
static inline uint16_t tespkt_mca_size (tespkt* pkt);
#else
static inline uint32_t tespkt_mca_size (tespkt* pkt);
#endif

/*
 * Get histogram's lowest value, most frequent bin, sum of all bins,
 * start and stop timestamp (only header frames).
 */
static inline uint32_t tespkt_mca_lvalue (tespkt* pkt);
static inline uint16_t tespkt_mca_mfreq  (tespkt* pkt);
static inline uint64_t tespkt_mca_total  (tespkt* pkt);
static inline uint64_t tespkt_mca_startt (tespkt* pkt);
static inline uint64_t tespkt_mca_stopt  (tespkt* pkt);

/*
 * Get bin number <bin> (starting at 0) of the current frame.
 */
static inline uint32_t tespkt_mca_bin (tespkt* pkt, uint16_t bin);

/*
 * Get MCA flags as a struct with separate fields for each register
 * (only header frames).
 */
static inline struct tespkt_mca_flags* tespkt_mca_fl (tespkt* pkt);

/* ---------- Call the following only on event frames. ---------- */
/*
 * Get number of events in an event frame.
 */
static inline uint16_t tespkt_event_nums (tespkt* pkt);

/*
 * Get event's size, type and time.
 */
static inline uint16_t tespkt_esize (tespkt* pkt); // in 8-bytes
static inline uint16_t tespkt_true_esize (tespkt* pkt); // in bytes
static inline struct tespkt_event_type* tespkt_etype (tespkt* pkt);
static inline uint16_t tespkt_event_toff (tespkt* pkt, uint16_t e);

/*
 * Get event's area (all but tick, peak and average trace).
 */
static inline uint32_t tespkt_event_area (tespkt* pkt, uint16_t e);

/*
 * Get tick's period, timestamp, error registers and events lost
 * (only tick frames).
 */
static inline uint32_t tespkt_tick_period (tespkt* pkt);
static inline uint64_t tespkt_tick_ts     (tespkt* pkt);
static inline uint8_t  tespkt_tick_ovrfl  (tespkt* pkt);
static inline uint8_t  tespkt_tick_err    (tespkt* pkt);
static inline uint8_t  tespkt_tick_cfd    (tespkt* pkt);
static inline uint32_t tespkt_tick_lost   (tespkt* pkt);

/*
 * Get peak's height and rise time (only peak frames).
 */
static inline uint16_t tespkt_peak_ht    (tespkt* pkt);
static inline uint16_t tespkt_peak_riset (tespkt* pkt);

/*
 * Get area's area (only area frames).
 */
static inline uint32_t tespkt_area_area  (tespkt* pkt);

/*
 * Get pulse's size, area, length, time offset (only pulse frames).
 */
static inline uint16_t tespkt_pulse_size (tespkt* pkt);
static inline uint32_t tespkt_pulse_area (tespkt* pkt);
static inline uint16_t tespkt_pulse_len  (tespkt* pkt);
static inline uint16_t tespkt_pulse_toff (tespkt* pkt);

/*
 * Get trace's size, area, length, time offset (only trace frames).
 */
static inline uint16_t tespkt_trace_size (tespkt* pkt);
static inline uint32_t tespkt_trace_area (tespkt* pkt);
static inline uint16_t tespkt_trace_len  (tespkt* pkt);
static inline uint16_t tespkt_trace_toff (tespkt* pkt);

/*
 * Get event (or tick) flags as a struct with separate fields for
 * each register.
 */
static inline struct tespkt_event_flags* tespkt_evt_fl (tespkt* pkt,
	uint16_t e);
static inline struct tespkt_tick_flags*  tespkt_tick_fl (tespkt* pkt);

/*
 * Get trace flags as a struct with separate fields for each
 * register (only trace frames).
 */
static inline struct tespkt_trace_flags* tespkt_trace_fl (tespkt* pkt);

/* -------------- Call the following on any frame. -------------- */

/*
 * Print info about packet.
 */
static void tespkt_pretty_print (tespkt* pkt,
		FILE* ostream, FILE* estream);

/*
 * Check if packet is valid.
 * Returns 0 if all is ok, or one or more OR-ed flags TES_E* flags.
 * Examine with tespkt_*error.
 */
static int  tespkt_is_valid (tespkt* pkt);

/*
 * Print info about each of the error bits set in err.
 */
static void tespkt_perror (FILE* stream, int err);

/*
 * As above, but return a pointer to a string literal describing the
 * error associated with the lowest error bit set in err. Caller
 * should clear the bit and call again.
 */
static const char* tespkt_error (int err);

/*
 * As above, but put the error associated with the lowest error
 * bit set in err in buf. buf must be able to hold at least
 * TES_EMAXLEN characters.
 * Returns the error with the corresponding bit cleared.
 */
static int  tespkt_serror (char* buf, int err);

#ifdef TESPKT_DEBUG
static void tespkt_self_test (void);
#endif

/*
 * Flags and event type are always sent as big-endian.
 */
struct tespkt_event_type
{
#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t TR  :  2;
	uint8_t     :  6; /* reserved */

	uint8_t     :  1; /* reserved */
	uint8_t T   :  1;
	uint8_t PKT :  2;
	uint8_t     :  4; /* reserved */
#else
	uint8_t     :  6; /* reserved */
	uint8_t TR  :  2;

	uint8_t     :  4; /* reserved */
	uint8_t PKT :  2;
	uint8_t T   :  1;
	uint8_t     :  1; /* reserved */
#endif
};

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

/* -------------------------------------------------------------- */

#define ETHERTYPE_F_EVENT 0x88B5
#define ETHERTYPE_F_MCA   0x88B6

#define TESPKT_TYPE_PEAK   0
#define TESPKT_TYPE_AREA   1
#define TESPKT_TYPE_PULSE  2
#define TESPKT_TYPE_TRACE  3
#define TESPKT_TRACE_TYPE_SGL  0
#define TESPKT_TRACE_TYPE_AVG  1
#define TESPKT_TRACE_TYPE_DP   2
#define TESPKT_TRACE_TYPE_DPTR 3

#define TESPKT_HDR_LEN       24 // including the ethernet header
#define TESPKT_MCA_HDR_LEN   40
#define TESPKT_TICK_HDR_LEN  24
#define TESPKT_PEAK_HDR_LEN   8
#define TESPKT_PEAK_LEN       8
#define TESPKT_AREA_HDR_LEN   8
#define TESPKT_PULSE_LEN      8
#define TESPKT_PULSE_HDR_LEN 16 // 8 + TESPKT_PULSE_LEN
#define TESPKT_TRACE_HDR_LEN  8
#define TESPKT_TRACE_FULL_HDR_LEN  16 // TESPKT_TRACE_HDR_LEN + TESPKT_PULSE_LEN
#define TESPKT_MCA_BIN_LEN    4
#define TESPKT_MTU         1496

struct tespkt_mca_hdr
{
	uint16_t size;
	uint16_t last_bin;
	uint32_t lowest_value;
	uint16_t : 16; /* reserved */
	uint16_t most_frequent;
	struct tespkt_mca_flags flags;
	uint64_t total;
	uint64_t start_time;
	uint64_t stop_time;
};

/* Access to flags and time in an event-type agnostic way */
struct tespkt_event_hdr
{
	uint32_t : 32;
	struct tespkt_event_flags flags;
	uint16_t toff;
};

struct tespkt_tick_hdr
{
	uint32_t period;
	struct tespkt_tick_flags flags;
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
	struct tespkt_event_flags flags;
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
	struct tespkt_event_flags flags;
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
	struct tespkt_event_flags flags;
	uint16_t toff;
	struct   tespkt_pulse pulse;
};

struct tespkt_trace_hdr
{
	uint16_t size;
	struct tespkt_trace_flags tr_flags;
	struct tespkt_event_flags flags;
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
		uint16_t length;             /* length of packet */
	} __attribute__ ((__packed__));
	struct
	{
		uint16_t fseq;
		uint16_t pseq;
		uint16_t esize;                 /* undefined for MCA */
		struct tespkt_event_type etype; /* undefined for MCA */
	} tes_hdr;
	void* body;
};

/* -------------------------------------------------------------- */
/* ------------------------- DEFINITIONS ------------------------ */
/* -------------------------------------------------------------- */

static inline char*
tespkt_dst_eth_ntoa (tespkt* pkt)
{
	return ether_ntoa (
		(struct ether_addr*) pkt->eth_hdr.ether_dhost);
}

static inline char*
tespkt_src_eth_ntoa (tespkt* pkt)
{
	return ether_ntoa (
		(struct ether_addr*) pkt->eth_hdr.ether_shost);
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

/* -------------------------------------------------------------- */

static inline int
tespkt_is_header (tespkt* pkt)
{ /* Byte order is irrelevant */
	return ( pkt->tes_hdr.pseq == 0 );
}

static inline int
tespkt_is_mca (tespkt* pkt)
{ /* ethernet type is always big-endian */
	return ( pkt->eth_hdr.ether_type == ntohs (ETHERTYPE_F_MCA) );
}

static inline int
tespkt_is_event (tespkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ntohs (ETHERTYPE_F_EVENT) );
}

static inline int
tespkt_is_tick (tespkt* pkt)
{
	return ( tespkt_is_event (pkt) &&
		 pkt->tes_hdr.etype.T == 1 );
}

static inline int
tespkt_is_peak (tespkt* pkt)
{
	return ( tespkt_is_event (pkt) &&
		 pkt->tes_hdr.etype.PKT == TESPKT_TYPE_PEAK &&
		 pkt->tes_hdr.etype.T == 0 );
}

static inline int
tespkt_is_area (tespkt* pkt)
{
	return ( tespkt_is_event (pkt) &&
		 pkt->tes_hdr.etype.PKT == TESPKT_TYPE_AREA &&
		 pkt->tes_hdr.etype.T == 0 );
}

static inline int
tespkt_is_pulse (tespkt* pkt)
{
	return ( tespkt_is_event (pkt) &&
		 pkt->tes_hdr.etype.PKT == TESPKT_TYPE_PULSE &&
		 pkt->tes_hdr.etype.T == 0 );
}

static inline int
tespkt_is_trace (tespkt* pkt)
{
	return ( tespkt_is_event (pkt) &&
		 pkt->tes_hdr.etype.PKT == TESPKT_TYPE_TRACE &&
		 pkt->tes_hdr.etype.T == 0 );
}

static inline int
tespkt_is_trace_long (tespkt* pkt)
{
	return ( tespkt_is_trace (pkt) &&
		! tespkt_is_trace_dp (pkt) );
}

static inline int
tespkt_is_trace_sgl (tespkt* pkt)
{
	return ( tespkt_is_event (pkt) &&
		 pkt->tes_hdr.etype.PKT == TESPKT_TYPE_TRACE &&
		 pkt->tes_hdr.etype.T == 0 &&
		 pkt->tes_hdr.etype.TR == TESPKT_TRACE_TYPE_SGL );
}

static inline int
tespkt_is_trace_avg (tespkt* pkt)
{
	return ( tespkt_is_event (pkt) &&
		 pkt->tes_hdr.etype.PKT == TESPKT_TYPE_TRACE &&
		 pkt->tes_hdr.etype.T == 0 &&
		 pkt->tes_hdr.etype.TR == TESPKT_TRACE_TYPE_AVG );
}

static inline int
tespkt_is_trace_dp (tespkt* pkt)
{
	return ( tespkt_is_event (pkt) &&
		 pkt->tes_hdr.etype.PKT == TESPKT_TYPE_TRACE &&
		 pkt->tes_hdr.etype.T == 0 &&
		 pkt->tes_hdr.etype.TR == TESPKT_TRACE_TYPE_DP );
}

static inline int
tespkt_is_trace_dptr (tespkt* pkt)
{
	return ( tespkt_is_event (pkt) &&
		 pkt->tes_hdr.etype.PKT == TESPKT_TYPE_TRACE &&
		 pkt->tes_hdr.etype.T == 0 &&
		 pkt->tes_hdr.etype.TR == TESPKT_TRACE_TYPE_DPTR );
}

/* -------------------------------------------------------------- */

static inline uint16_t
tespkt_mca_nbins (tespkt* pkt)
{
	if (tespkt_is_header (pkt))
		return ( (tespkt_flen (pkt) - TESPKT_HDR_LEN - TESPKT_MCA_HDR_LEN)
			/ TESPKT_MCA_BIN_LEN );
	else
		return ( (tespkt_flen (pkt) - TESPKT_HDR_LEN)
			/ TESPKT_MCA_BIN_LEN );
}

static inline uint16_t
tespkt_mca_nbins_tot (tespkt* pkt)
{
	return ftohl (
		((struct tespkt_mca_hdr*) &pkt->body)->last_bin + 1);
}

#ifndef TES_MCASIZE_BUG
static inline uint16_t
tespkt_mca_size (tespkt* pkt)
{
	return ftohl (
		((struct tespkt_mca_hdr*) &pkt->body)->size);
}
#else
static inline uint32_t
tespkt_mca_size (tespkt* pkt)
{
	return ( (tespkt_mca_nbins_tot (pkt) * TESPKT_MCA_BIN_LEN) + TESPKT_MCA_HDR_LEN );
}
#endif

static inline uint32_t
tespkt_mca_lvalue (tespkt* pkt)
{
	return ftohl (
		((struct tespkt_mca_hdr*) &pkt->body)->lowest_value);
}

static inline uint16_t
tespkt_mca_mfreq (tespkt* pkt)
{
	return ftohl (
		((struct tespkt_mca_hdr*) &pkt->body)->most_frequent);
}

static inline uint64_t
tespkt_mca_total (tespkt* pkt)
{
	return ftohl (
		((struct tespkt_mca_hdr*) &pkt->body)->total);
}

static inline uint64_t
tespkt_mca_startt (tespkt* pkt)
{
	return ftohl (
		((struct tespkt_mca_hdr*) &pkt->body)->start_time);
}

static inline uint64_t
tespkt_mca_stopt (tespkt* pkt)
{
	return ftohl (
		((struct tespkt_mca_hdr*) &pkt->body)->stop_time);
}

static inline uint32_t
tespkt_mca_bin (tespkt* pkt, uint16_t bin)
{
	if (tespkt_is_header (pkt))
		return ftohl *(uint32_t*)(
			(char*)&pkt->body + bin*TESPKT_MCA_BIN_LEN + TESPKT_MCA_HDR_LEN);
	else
		return ftohl *(uint32_t*)(
			(char*)&pkt->body + bin*TESPKT_MCA_BIN_LEN);
}

static inline struct tespkt_mca_flags*
tespkt_mca_fl (tespkt* pkt)
{
	struct tespkt_mca_hdr* mh = 
	   (struct tespkt_mca_hdr*) &pkt->body;
	return &mh->flags;
}

/* -------------------------------------------------------------- */

static inline uint16_t
tespkt_event_nums (tespkt* pkt)
{
	if (tespkt_is_trace (pkt))
		return ( (tespkt_is_header (pkt) ||
			tespkt_is_trace_dp (pkt)) ? 1 : 0);
	return ( ( tespkt_flen (pkt) - TESPKT_HDR_LEN ) / (
		tespkt_true_esize (pkt) ) );
}

static inline uint16_t
tespkt_esize (tespkt* pkt)
{
	return ftohs (pkt->tes_hdr.esize);
}

static inline uint16_t
tespkt_true_esize (tespkt* pkt)
{
	return (ftohs (pkt->tes_hdr.esize) << 3);
}

static inline struct tespkt_event_type*
tespkt_etype  (tespkt* pkt)
{
	return &pkt->tes_hdr.etype;
}

static inline uint16_t
tespkt_event_toff (tespkt* pkt, uint16_t e)
{
	return ftohs ( ((struct tespkt_event_hdr*) (
		(char*)&pkt->body + e*tespkt_true_esize (pkt) ))->toff);
}

/* tespkt_pulse_hdr is compatible with tespkt_trace_full_hdr */
static inline uint32_t
tespkt_event_area (tespkt* pkt, uint16_t e)
{
	if (tespkt_is_area (pkt))
		return ftohs (
			((struct tespkt_area_hdr*) &pkt->body)->area);
	else
		return ftohs (
			((struct tespkt_pulse_hdr*) &pkt->body)->pulse.area);
}

static inline uint32_t
tespkt_tick_period (tespkt* pkt)
{
	return ftohs (
		((struct tespkt_tick_hdr*) &pkt->body)->period);
}

static inline uint64_t
tespkt_tick_ts (tespkt* pkt)
{
	return ftohs (
		((struct tespkt_tick_hdr*) &pkt->body)->ts);
}

static inline uint8_t
tespkt_tick_ovrfl (tespkt* pkt)
{
	return ftohs (
		((struct tespkt_tick_hdr*) &pkt->body)->ovrfl);
}

static inline uint8_t
tespkt_tick_err (tespkt* pkt)
{
	return ftohs (
		((struct tespkt_tick_hdr*) &pkt->body)->err);
}

static inline uint8_t
tespkt_tick_cfd (tespkt* pkt)
{
	return ftohs (
		((struct tespkt_tick_hdr*) &pkt->body)->cfd);
}

static inline uint32_t
tespkt_tick_lost (tespkt* pkt)
{
	return ftohs (
		((struct tespkt_tick_hdr*) &pkt->body)->lost);
}

static inline uint16_t
tespkt_peak_ht (tespkt* pkt)
{
	return ftohs (
		((struct tespkt_peak_hdr*) &pkt->body)->height);
}

static inline uint16_t
tespkt_peak_riset (tespkt* pkt)
{
	return ftohs (
		((struct tespkt_peak_hdr*) &pkt->body)->rise_time);
}

static inline uint32_t
tespkt_area_area (tespkt* pkt)
{
	return ftohs (
		((struct tespkt_area_hdr*) &pkt->body)->area);
}

static inline uint16_t
tespkt_pulse_size (tespkt* pkt)
{
	return ftohs (
		((struct tespkt_pulse_hdr*) &pkt->body)->size);
}

static inline uint32_t
tespkt_pulse_area (tespkt* pkt)
{
	return ftohs (
		((struct tespkt_pulse_hdr*) &pkt->body)->pulse.area);
}

static inline uint16_t
tespkt_pulse_len (tespkt* pkt)
{
	return ftohs (
		((struct tespkt_pulse_hdr*) &pkt->body)->pulse.length);
}

static inline uint16_t
tespkt_pulse_toff (tespkt* pkt)
{
	return ftohs (
		((struct tespkt_pulse_hdr*) &pkt->body)->pulse.toffset);
}

static inline uint16_t
tespkt_trace_size (tespkt* pkt)
{
	return ftohs (
		((struct tespkt_trace_hdr*) &pkt->body)->size);
}

static inline uint32_t
tespkt_trace_area (tespkt* pkt)
{
	return ftohs (
		((struct tespkt_trace_full_hdr*) &pkt->body)->pulse.area);
}

static inline uint16_t
tespkt_trace_len (tespkt* pkt)
{
	return ftohs (
		((struct tespkt_trace_full_hdr*) &pkt->body)->pulse.length);
}

static inline uint16_t
tespkt_trace_toff (tespkt* pkt)
{
	return ftohs (
		((struct tespkt_trace_full_hdr*) &pkt->body)->pulse.toffset);
}

static inline struct tespkt_event_flags*
tespkt_evt_fl (tespkt* pkt, uint16_t e)
{
	struct tespkt_event_hdr* eh = (struct tespkt_event_hdr*) (
		(char*)&pkt->body + e*tespkt_true_esize (pkt) );
	return &eh->flags;
}

static inline struct tespkt_tick_flags*
tespkt_tick_fl (tespkt* pkt)
{
	struct tespkt_tick_hdr* eh =
		(struct tespkt_tick_hdr*) &pkt->body;
	return &eh->flags;
}

static inline struct tespkt_trace_flags*
tespkt_trace_fl (tespkt* pkt)
{
	struct tespkt_trace_hdr* th =
		(struct tespkt_trace_hdr*) &pkt->body;
	return &th->tr_flags;
}

/* -------------------------------------------------------------- */

static void
tespkt_pretty_print (tespkt* pkt, FILE* ostream, FILE* estream)
{
	if (estream == NULL)
		estream = ostream;

	fprintf (ostream, "Destination MAC:     %s\n",
		tespkt_dst_eth_ntoa (pkt));
	fprintf (ostream, "Source MAC:          %s\n",
		tespkt_src_eth_ntoa (pkt));
	fprintf (ostream, "Packet length:       %hu\n",
		tespkt_flen (pkt));
	fprintf (ostream, "Frame sequence:      %hu\n",
		tespkt_fseq (pkt));
	fprintf (ostream, "Protocol sequence:   %hu\n",
		tespkt_pseq (pkt));

	/* ----- MCA */
	if (tespkt_is_mca (pkt))
	{
		fprintf (ostream, "Stream type:         MCA\n");
		fprintf (ostream, "Number of bins:      %u\n",
			tespkt_mca_nbins (pkt));
		if (!tespkt_is_header (pkt))
			return;
#ifndef TES_MCASIZE_BUG
		fprintf (ostream, "Size:                %hu\n",
			tespkt_mca_size (pkt));
#else
		fprintf (ostream, "Size:                %u\n",
			tespkt_mca_size (pkt));
#endif
		struct tespkt_mca_flags* mf = tespkt_mca_fl (pkt);
		fprintf (ostream, "Flag Q:              %hhu\n", mf->Q);
		fprintf (ostream, "Flag V:              %hhu\n", mf->V);
		fprintf (ostream, "Flag T:              %hhu\n", mf->T);
		fprintf (ostream, "Flag N:              %hhu\n", mf->N);
		fprintf (ostream, "Flag C:              %hhu\n", mf->C);
		fprintf (ostream, "Total number of bins:%u\n",
			tespkt_mca_nbins_tot (pkt));
		fprintf (ostream, "Lowest value:        %u\n",
			tespkt_mca_lvalue (pkt));
		fprintf (ostream, "Most frequent bin:   %hu\n",
			tespkt_mca_mfreq (pkt));
		fprintf (ostream, "Total:               %lu\n",
			tespkt_mca_total (pkt));
		fprintf (ostream, "Start time:          %lu\n",
			tespkt_mca_startt (pkt));
		fprintf (ostream, "Stop time:           %lu\n",
			tespkt_mca_stopt (pkt));
		return;
	}
	if (!tespkt_is_event (pkt))
	{
		fprintf (estream, "Unknown stream type\n");
		return;
	}

	/* ----- Event */
	fprintf (ostream, "Stream type:         Event\n");
	fprintf (ostream, "Event size:          %hu\n",
		tespkt_esize (pkt));
	fprintf (ostream, "Number of events:    %hu\n",
		tespkt_event_nums (pkt));
	/* ---------- Tick event */
	if (tespkt_is_tick (pkt))
	{
		struct tespkt_tick_flags* tf = tespkt_tick_fl (pkt);
		fprintf (ostream, "Tick flag MF:        %hhu\n", tf->MF);
		fprintf (ostream, "Tick flag EL:        %hhu\n", tf->EL);
		fprintf (ostream, "Tick flag TL:        %hhu\n", tf->TL);
		fprintf (ostream, "Tick flag T:         %hhu\n", tf->T);
		fprintf (ostream, "Tick flag N:         %hhu\n", tf->N);
		fprintf (ostream, "Period:              %u\n",
			tespkt_tick_period (pkt));
		fprintf (ostream, "Timestamp:           %lu\n",
			tespkt_tick_ts (pkt));
		fprintf (ostream, "Error ovrfl:         %hhu\n",
			tespkt_tick_ovrfl (pkt));
		fprintf (ostream, "Error err:           %hhu\n",
			tespkt_tick_err (pkt));
		fprintf (ostream, "Error cfd:           %hhu\n",
			tespkt_tick_cfd (pkt));
		fprintf (ostream, "Events lost:         %u\n",
			tespkt_tick_lost (pkt));
		fprintf (ostream, "Type:                Tick\n");
		return;
	}
	/* ---------- Non-tick event */
	for (int e = 0; e < tespkt_event_nums (pkt); e++)
	{
		fprintf (ostream, "Event time offset:   %hhu\n",
			tespkt_event_toff (pkt, e));
		struct tespkt_event_flags* ef = tespkt_evt_fl (pkt, e);
		fprintf (ostream, "Event flag PC:       %hhu\n", ef->PC);
		fprintf (ostream, "Event flag O:        %hhu\n", ef->O);
		fprintf (ostream, "Event flag CH:       %hhu\n", ef->CH);
		fprintf (ostream, "Event flag TT:       %hhu\n", ef->TT);
		fprintf (ostream, "Event flag HT:       %hhu\n", ef->HT);
		fprintf (ostream, "Event flag PT:       %hhu\n", ef->PT);
		fprintf (ostream, "Event flag T:        %hhu\n", ef->T);
		fprintf (ostream, "Event flag N:        %hhu\n", ef->N);
	}
	/* --------------- Peak */
	if (tespkt_is_peak (pkt))
	{
		fprintf (ostream, "Type:                Peak\n");
		fprintf (ostream, "Height:              %hu\n",
			tespkt_peak_ht (pkt));
		fprintf (ostream, "Rise time:           %hu\n",
			tespkt_peak_riset (pkt));
		return;
	}
	/* --------------- Area */
	if (tespkt_is_area (pkt))
	{
		fprintf (ostream, "Type:                Area\n");
		fprintf (ostream, "Area:                %u\n",
			tespkt_area_area (pkt));
		return;
	}
	/* --------------- Pulse */
	if (tespkt_is_pulse (pkt))
	{
		fprintf (ostream, "Type:                Pulse\n");
		fprintf (ostream, "Size:                %hu\n",
			tespkt_pulse_size (pkt));
		fprintf (ostream, "Area:                %u\n",
			tespkt_pulse_area (pkt));
		fprintf (ostream, "Length:              %hu\n",
			tespkt_pulse_len (pkt));
		fprintf (ostream, "Time offset:         %hu\n",
			tespkt_pulse_toff (pkt));
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
	fprintf (ostream, "Trace size:          %hu\n",
		tespkt_trace_size (pkt));
	/* -------------------- Average */
	if (tespkt_is_trace_avg (pkt))
	{
		fprintf (ostream, "Trace type:          Average\n");
		return;
	}
	fprintf (ostream, "Area:                %u\n",
		tespkt_trace_area (pkt));
	fprintf (ostream, "Length:              %hu\n",
		tespkt_trace_len (pkt));
	fprintf (ostream, "Time offset:         %hu\n",
		tespkt_trace_toff (pkt));
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
		return;
	}
	/* -------------------- Dot product + trace */
	if (tespkt_is_trace_dptr (pkt))
	{
		fprintf (ostream, "Trace type:          Dot product with trace\n");
		return;
	}
	fprintf (estream, "Unknown trace type\n");
}

/* Return codes */
static const char* _tespkt_errors[7] = {
#define TES_EETHTYPE    1 // ether type 
	"Invalid ether type",
#define TES_EETHLEN     2 // frame length
	"Invalid frame length",
#define TES_EEVTTYPE    4 // event type 
	"Invalid event type",
#define TES_EEVTSIZE    8 // event size for fixed size events
	"Invalid event size",
#define TES_ETRSIZE    16 // event size for fixed size events
	"Invalid trace size",
#define TES_EMCASIZE   32 // mismatch: size vs last bin
	"Invalid histogram size",
#define TES_EMCABINS   64 // mismatch: most frequent vs last bin
	"Invalid bin number in histogram",
};

#define TES_EMAXLEN     64 // maximum length of error string

static int
tespkt_is_valid (tespkt* pkt)
{
	int rc = 0;

	uint16_t flen = tespkt_flen (pkt);

	/* Frame length should be a multiple of 8 */
	if (flen & 7 || flen > TESPKT_MTU)
		rc |= TES_EETHLEN;
	/* and it should be more than the header length. */
	if (flen <= TESPKT_HDR_LEN)
		rc |= TES_EETHLEN;

	if (tespkt_is_event (pkt))
	{
		uint16_t esize = tespkt_esize (pkt);

		/* Event size should not be 0 and payload length should be
		 * a multiple of event size * 8. */
		if (esize == 0)
			rc |= TES_EEVTSIZE;
		else if ( (flen - TESPKT_HDR_LEN) % (esize << 3) != 0 )
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
			if (tespkt_is_header (pkt))
			{
				uint16_t trsize = tespkt_trace_size (pkt);
				/* Trace size should not be 0 */
				if (trsize == 0)
					rc |= TES_ETRSIZE;
				/* and it should not be smaller than the
				 * payload length */
				if (flen - TESPKT_HDR_LEN > trsize)
					rc |= TES_ETRSIZE;
			}

			if ( ( ! tespkt_is_trace_dp (pkt) ) && esize != 1 )
				rc |= TES_EEVTSIZE;
		}
		else if (!tespkt_is_pulse (pkt))
			rc |= TES_EEVTTYPE;
	}
	else if (tespkt_is_mca (pkt))
	{
		if (tespkt_is_header (pkt))
		{
			uint16_t nbins_tot = tespkt_mca_nbins_tot (pkt);
#ifndef TES_MCASIZE_BUG
			uint16_t histsize = tespkt_mca_size (pkt);
			/* MCA size should correspond to last bin. */
			if (histsize != (nbins_tot * TESPKT_MCA_BIN_LEN) + TESPKT_MCA_HDR_LEN)
				rc |= TES_EMCASIZE;
			/* and it should not be smaller than the
			 * payload length */
			if (flen - TESPKT_HDR_LEN > histsize)
				rc |= TES_EMCASIZE;
#else
			uint32_t histsize = tespkt_mca_size (pkt);
			if ((uint32_t)flen - TESPKT_HDR_LEN > histsize)
				rc |= TES_EMCASIZE;
#endif

			/* Most frequent bin cannot be greater than last
			 * bin. */
			if (tespkt_mca_mfreq (pkt) >= nbins_tot)
				rc |= TES_EMCABINS;
		}
	}
	else
		rc |= TES_EETHTYPE;

	return rc;
}

static void
tespkt_perror (FILE* stream, int err)
{
	for (int e = 0; err != 0; err >>= 1, e++)
		if (err & 1)
			fprintf (stream, "%s\n", _tespkt_errors[e]);
}

static const char*
tespkt_error (int err)
{
	for (int e = 0; err != 0; err >>= 1, e++)
		if (err & 1)
			return _tespkt_errors[e];

	return NULL; /* suppress gcc warning */
}

static int
tespkt_serror (char* buf, int err)
{
	for (int e = 0; err != 0; err >>= 1, e++)
	{
		if (err & 1)
		{
			snprintf (buf, TES_EMAXLEN, "%s", _tespkt_errors[e]);
			return err;
		}
	}

	return 0; /* suppress gcc warning */
}

/* -------------------------------------------------------------- */
/* ---------------------------- DEBUG --------------------------- */
/* -------------------------------------------------------------- */

#ifdef TESPKT_DEBUG

#define TESPKT_MCA_FLAGS_MASK   0x000fffff
#define TESPKT_EVT_FLAGS_MASK   0xffff
#define TESPKT_TICK_FLAGS_MASK  0x0703
#define TESPKT_TRACE_FLAGS_MASK 0x7fff

/*
 * Event types are sent as separate bytes, i.e. always appear
 * big-endian.
 */
#define TESPKT_EVT_TYPE_MASK     0x030e /* all relevant bits of etype */
#define TESPKT_EVT_PKT_TYPE_MASK 0x000e /* packet type and tick bits */

static void
tespkt_self_test (void)
{
	assert (offsetof (tespkt, body) == TESPKT_HDR_LEN);
	assert (sizeof (struct tespkt_mca_hdr) == TESPKT_MCA_HDR_LEN);
	assert (sizeof (struct tespkt_tick_hdr) == TESPKT_TICK_HDR_LEN);
	assert (sizeof (struct tespkt_peak_hdr) == TESPKT_PEAK_HDR_LEN);
	assert (sizeof (struct tespkt_peak) == TESPKT_PEAK_LEN);
	assert (sizeof (struct tespkt_area_hdr) == TESPKT_AREA_HDR_LEN);
	assert (sizeof (struct tespkt_pulse) == TESPKT_PULSE_LEN);
	assert (sizeof (struct tespkt_pulse_hdr) == TESPKT_PULSE_HDR_LEN);
	assert (sizeof (struct tespkt_trace_hdr) == TESPKT_TRACE_HDR_LEN);
	assert (sizeof (struct tespkt_trace_full_hdr) == TESPKT_TRACE_FULL_HDR_LEN);

	for (unsigned int e = 0;
		e < sizeof (_tespkt_errors) / sizeof (char*) ; e++)
		assert (tespkt_error (1 << e) == _tespkt_errors[e]);

	/* Check the order of the bitfields in the structs */
	union
	{
		struct tespkt_event_type fields;
		uint16_t val;
	} et;
	memset (&et, 0, sizeof (struct tespkt_event_type));
	et.fields.T = 1;
	et.fields.PKT = 3;
	assert (ntohs (et.val) == TESPKT_EVT_PKT_TYPE_MASK);
	et.fields.TR = 3;
	assert (ntohs (et.val) == TESPKT_EVT_TYPE_MASK);

	union
	{
		struct tespkt_mca_flags fields;
		uint32_t val;
	} mf;
	memset (&mf, 0, sizeof (struct tespkt_mca_flags));
	mf.fields.Q = 0x0f;
	mf.fields.V = 0x0f;
	mf.fields.T = 0x0f;
	mf.fields.N = 0x1f;
	mf.fields.C = 0x07;
	assert (ntohl (mf.val) == TESPKT_MCA_FLAGS_MASK);

	union
	{
		struct tespkt_event_flags fields;
		uint16_t val;
	} ef;
	memset (&ef, 0, sizeof (struct tespkt_event_flags));
	ef.fields.PC = 0x0f;
	ef.fields.O  = 0x01;
	ef.fields.CH = 0x07;
	ef.fields.TT = 0x03;
	ef.fields.HT = 0x03;
	ef.fields.PT = 0x03;
	ef.fields.T  = 0x01;
	ef.fields.N  = 0x01;
	assert (ntohs (ef.val) == TESPKT_EVT_FLAGS_MASK);

	union
	{
		struct tespkt_tick_flags fields;
		uint16_t val;
	} tf;
	memset (&tf, 0, sizeof (struct tespkt_tick_flags));
	tf.fields.MF = 0x01;
	tf.fields.EL = 0x01;
	tf.fields.TL = 0x01;
	tf.fields.T  = 0x01;
	tf.fields.N  = 0x01;
	assert (ntohs (tf.val) == TESPKT_TICK_FLAGS_MASK);

	union
	{
		struct tespkt_trace_flags fields;
		uint16_t val;
	} trf;
	memset (&trf, 0, sizeof (struct tespkt_trace_flags));
	trf.fields.MH  = 0x01;
	trf.fields.MP  = 0x01;
	trf.fields.STR = 0x1f;
	trf.fields.TT  = 0x03;
	trf.fields.TS  = 0x03;
	trf.fields.OFF = 0x0f;
	assert (ntohs (trf.val) == TESPKT_TRACE_FLAGS_MASK);
}

#endif /* TESPKT_DEBUG */

#endif
