#ifndef __NET_TESPKT_H_INCLUDED__
#define __NET_TESPKT_H_INCLUDED__

/*
 * One can either use struct tespkt directly and access its fields (note
 * that byte order needs to be taken into account then), or use the helper
 * functions below which take a pointer to it and change the byte order
 * where necessary. For convenience a type tespkt is aliased to struct
 * tespkt
 */

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
static inline int tespkt_is_event   (tespkt* pkt);
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
static inline uint16_t tespkt_esize (tespkt* pkt);
/* Number of events in an event frame */
static inline uint16_t tespkt_event_nums (tespkt* pkt);
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
/* Bin number <bin> (starting at 0) of the current frame */
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
/* Event type */
static inline struct tespkt_event_type*   tespkt_etype  (tespkt* pkt);
/* Event's time (valid for all events) */
static inline uint16_t tespkt_event_toff (tespkt* pkt);
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
/* Pulse's size, area, length, time offset (valid for pulse events) */
static inline uint16_t tespkt_pulse_size (tespkt* pkt);
static inline uint32_t tespkt_pulse_area (tespkt* pkt);
static inline uint16_t tespkt_pulse_len  (tespkt* pkt);
static inline uint16_t tespkt_pulse_toff (tespkt* pkt);
/* Trace's size (valid for all trace events except average) */
static inline uint16_t tespkt_trace_size (tespkt* pkt);
/* Trace's area, length, time offset (valid for all trace events) */
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
	uint8_t SEQ :  1; /* set by us */
	uint8_t BAD :  1; /* set by us */
	uint8_t MCA :  1; /* set by us */
	uint8_t HOM :  1; /* set by us */
#else
	uint8_t     :  6; /* reserved */
	uint8_t TR  :  2;

	uint8_t HOM :  1; /* set by us */
	uint8_t MCA :  1; /* set by us */
	uint8_t BAD :  1; /* set by us */
	uint8_t SEQ :  1; /* set by us */
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

/* ------------------------------------------------------------------------- */

#define ETHERTYPE_F_EVENT 0x88B5
#define ETHERTYPE_F_MCA   0x88B6

#define PKT_TYPE_PEAK   0
#define PKT_TYPE_AREA   1
#define PKT_TYPE_PULSE  2
#define PKT_TYPE_TRACE  3
#define TRACE_TYPE_SGL  0
#define TRACE_TYPE_AVG  1
#define TRACE_TYPE_DP   2
#define TRACE_TYPE_DPTR 3

#define TES_HDR_LEN         24 /* includes the 16 byte ethernet header */
#define MCA_HDR_LEN         40
#define TICK_HDR_LEN        24
#define PEAK_HDR_LEN         8
#define PEAK_LEN             8
#define AREA_HDR_LEN         8
#define PULSE_LEN            8
#define PULSE_HDR_LEN       16 // 8 + PULSE_LEN
#define TRACE_HDR_LEN        8
#define TRACE_FULL_HDR_LEN  16 // TRACE_HDR_LEN + PULSE_LEN
#define DP_LEN               8
#define SMPL_LEN             2
#define BIN_LEN              4
#define MAX_TES_FRAME_LEN 1496

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

/* Used to access flags and time in an event-type agnostic way */
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
		uint16_t length;	     /* length of packet */
	};
	struct
	{
		uint16_t fseq;
		uint16_t pseq;
		uint16_t esize; /* undefined for MCA frames */
		struct tespkt_event_type etype; /* undefined for MCA frames */
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
		 pkt->tes_hdr.etype.PKT == PKT_TYPE_PEAK &&
		 pkt->tes_hdr.etype.T == 0 );
}

static inline int
tespkt_is_area (tespkt* pkt)
{
	return ( tespkt_is_event (pkt) &&
		 pkt->tes_hdr.etype.PKT == PKT_TYPE_AREA &&
		 pkt->tes_hdr.etype.T == 0 );
}

static inline int
tespkt_is_pulse (tespkt* pkt)
{
	return ( tespkt_is_event (pkt) &&
		 pkt->tes_hdr.etype.PKT == PKT_TYPE_PULSE &&
		 pkt->tes_hdr.etype.T == 0 );
}

static inline int
tespkt_is_trace (tespkt* pkt)
{
	return ( tespkt_is_event (pkt) &&
		 pkt->tes_hdr.etype.PKT == PKT_TYPE_TRACE &&
		 pkt->tes_hdr.etype.T == 0 );
}

static inline int
tespkt_is_trace_sgl (tespkt* pkt)
{
	return ( tespkt_is_event (pkt) &&
		 pkt->tes_hdr.etype.PKT == PKT_TYPE_TRACE &&
		 pkt->tes_hdr.etype.T == 0 &&
		 pkt->tes_hdr.etype.T == TRACE_TYPE_SGL );
}

static inline int
tespkt_is_trace_avg (tespkt* pkt)
{
	return ( tespkt_is_event (pkt) &&
		 pkt->tes_hdr.etype.PKT == PKT_TYPE_TRACE &&
		 pkt->tes_hdr.etype.T == 0 &&
		 pkt->tes_hdr.etype.T == TRACE_TYPE_AVG );
}

static inline int
tespkt_is_trace_dp (tespkt* pkt)
{
	return ( tespkt_is_event (pkt) &&
		 pkt->tes_hdr.etype.PKT == PKT_TYPE_TRACE &&
		 pkt->tes_hdr.etype.T == 0 &&
		 pkt->tes_hdr.etype.T == TRACE_TYPE_DP );
}

static inline int
tespkt_is_trace_dptr (tespkt* pkt)
{
	return ( tespkt_is_event (pkt) &&
		 pkt->tes_hdr.etype.PKT == PKT_TYPE_TRACE &&
		 pkt->tes_hdr.etype.T == 0 &&
		 pkt->tes_hdr.etype.T == TRACE_TYPE_DPTR );
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
tespkt_esize (tespkt* pkt)
{
	return ftohs (pkt->tes_hdr.esize);
}

static inline uint16_t
tespkt_event_nums (tespkt* pkt)
{
	return ( ( tespkt_flen (pkt) - TES_HDR_LEN )
			/ ( tespkt_esize (pkt) << 3 ) );
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
	return &mh->flags;
}

static inline struct tespkt_event_flags*
tespkt_evt_fl (tespkt* pkt)
{
	struct tespkt_event_hdr* eh = (struct tespkt_event_hdr*)(void*) &pkt->body;
	return &eh->flags;
}

static inline struct tespkt_tick_flags*
tespkt_tick_fl (tespkt* pkt)
{
	struct tespkt_tick_hdr* eh = (struct tespkt_tick_hdr*)(void*) &pkt->body;
	return &eh->flags;
}

static inline struct tespkt_trace_flags*
tespkt_trace_fl (tespkt* pkt)
{
	struct tespkt_trace_hdr* th = (struct tespkt_trace_hdr*)(void*) &pkt->body;
	return &th->tr_flags;
}

static inline struct tespkt_event_type*
tespkt_etype  (tespkt* pkt)
{
	return &pkt->tes_hdr.etype;
}

static inline uint16_t
tespkt_event_toff (tespkt* pkt)
{
	return ftohs (((struct tespkt_event_hdr*)(void*) &pkt->body)->toff);
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
	if (!tespkt_is_event (pkt))
	{
		fprintf (estream, "Unknown stream type\n");
		return;
	}

	/* ----- Event */
	fprintf (ostream, "Stream type:         Event\n");
	fprintf (ostream, "Event size:          %hu\n", tespkt_esize (pkt));
	fprintf (ostream, "Time offset:         %hu\n", tespkt_event_toff (pkt));
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
#define TES_EETHTYPE    1 // ether type 
#define TES_EETHLEN     2 // frame length
#define TES_EEVTTYPE    4 // event type 
#define TES_EEVTSIZE    8 // event size for fixed size events
#define TES_ETRSIZE    16 // event size for fixed size events
#define TES_EMCASIZE   32 // mismatch: size vs last bin
#define TES_EMCABINS   64 // mismatch: most frequent vs last bin

#define TES_EETHTYPE_S  "Invalid ether type"
#define TES_EETHLEN_S   "Invalid frame length"
#define TES_EEVTTYPE_S  "Invalid event type"
#define TES_EEVTSIZE_S  "Invalid event size"
#define TES_ETRSIZE_S   "Invalid trace size"
#define TES_EMCASIZE_S  "Invalid histogram size"
#define TES_EMCABINS_S  "Invalid bin number in histogram"
#define TES_EMAXLEN     64 // maximum length of error string

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

	if (tespkt_is_event (pkt))
	{
		uint16_t esize = tespkt_esize (pkt);

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
			if (tespkt_is_header (pkt))
			{
				uint16_t trsize = tespkt_trace_size (pkt);
				/* Trace size should not be 0 */
				if (trsize == 0)
					rc |= TES_ETRSIZE;
				/* and it should not be smaller than the
				 * payload length */
				if (tespkt_flen (pkt) - TES_HDR_LEN > trsize)
					rc |= TES_ETRSIZE;
			}

			if ( ( ! tespkt_is_trace_dp (pkt) )
					&& esize != 1 )
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
			uint16_t histsize = tespkt_mca_size (pkt);
			/* MCA size should correspond to last bin. */
			if (histsize != (nbins_tot * BIN_LEN) + MCA_HDR_LEN)
				rc |= TES_EMCASIZE;
			/* and it should not be smaller than the
			 * payload length */
			if (tespkt_flen (pkt) - TES_HDR_LEN > histsize)
				rc |= TES_EMCASIZE;

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
}

/* ------------------------------------------------------------------------- */
/* --------------------------------- DEBUG --------------------------------- */
/* ------------------------------------------------------------------------- */

#ifdef TESPKT_DEBUG
#include <string.h>
#include <assert.h>

#define EVT_TYPE_LEN  2
#define MCA_FL_LEN    4
#define EVT_FL_LEN    2
#define TICK_FL_LEN   2
#define TRACE_FL_LEN  2

#define MCA_FL_MASK   0x000fffff
#define EVT_FL_MASK   0xffff
#define TICK_FL_MASK  0x0703
#define TRACE_FL_MASK 0x7fff

/*
 * Event types are sent as separate bytes, i.e. always appear big-endian.
 */
#define EVT_TYPE_MASK       0x030e /* all relevant bits of etype */
#define EVT_PKT_TYPE_MASK   0x000e /* the packet type and tick bits */
#define EVT_TYPE_TICK       0x0002
#define EVT_TYPE_PEAK       0x0000
#define EVT_TYPE_AREA       0x0004
#define EVT_TYPE_PULSE      0x0008
#define EVT_TYPE_TRACE      0x000c
#define EVT_TYPE_TRACE_SGL  0x000c
#define EVT_TYPE_TRACE_AVG  0x010c
#define EVT_TYPE_TRACE_DP   0x020c
#define EVT_TYPE_TRACE_DPTR 0x030c

/* Flags and event type are always sent as big-endian. */
#define structtoul(s_ptr) ntohl ( *( (uint32_t*) (void*) s_ptr ) )
#define structtous(s_ptr) ntohs ( *( (uint16_t*) (void*) s_ptr ) )

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
	assert (sizeof (struct tespkt_pulse) == PULSE_LEN);
	assert (sizeof (struct tespkt_pulse_hdr) == PULSE_HDR_LEN);
	assert (sizeof (struct tespkt_trace_hdr) == TRACE_HDR_LEN);
	assert (sizeof (struct tespkt_trace_full_hdr) == TRACE_FULL_HDR_LEN);
	assert (sizeof (struct tespkt_dot_prod) == DP_LEN);
	assert (sizeof (struct tespkt_event_type) == EVT_TYPE_LEN);
	assert (sizeof (struct tespkt_mca_flags) == MCA_FL_LEN);
	assert (sizeof (struct tespkt_event_flags) == EVT_FL_LEN);
	assert (sizeof (struct tespkt_tick_flags) == TICK_FL_LEN);
	assert (sizeof (struct tespkt_trace_flags) == TRACE_FL_LEN);

	struct tespkt_event_type et;
	memset (&et, 0, EVT_TYPE_LEN);
	et.T = 1;
	et.PKT = 3;
	assert ( structtous (&et) == EVT_PKT_TYPE_MASK );
	et.TR = 3;
	assert ( structtous (&et) == EVT_TYPE_MASK );

	struct tespkt_mca_flags mf;
	memset (&mf, 0, MCA_FL_LEN);
	mf.Q = 0x0f;
	mf.V = 0x0f;
	mf.T = 0x0f;
	mf.N = 0x1f;
	mf.C = 0x07;
	assert ( structtoul (&mf) == MCA_FL_MASK );

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
	assert ( structtous (&ef) == EVT_FL_MASK );

	struct tespkt_tick_flags tf;
	memset (&tf, 0, TICK_FL_LEN);
	tf.MF = 0x01;
	tf.EL = 0x01;
	tf.TL = 0x01;
	tf.T  = 0x01;
	tf.N  = 0x01;
	assert ( structtous (&tf) == TICK_FL_MASK );

	struct tespkt_trace_flags trf;
	memset (&trf, 0, TRACE_FL_LEN);
	trf.MH  = 0x01;
	trf.MP  = 0x01;
	trf.STR = 0x1f;
	trf.TT  = 0x03;
	trf.TS  = 0x03;
	trf.OFF = 0x0f;
	assert ( structtous (&trf) == TRACE_FL_MASK );
}

#endif /* TESPKT_DEBUG */

#endif
