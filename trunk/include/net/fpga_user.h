#ifndef __NET_FPGA_USER_H_INCLUDED__
#define __NET_FPGA_USER_H_INCLUDED__

#ifdef FPGA_DEBUG
/* net/fpga.h will use this test, forward delcare it here */
static void __fpga_user_self_test ();
#endif

#include <net/fpga.h>

typedef struct __fpga_pkt fpga_pkt;

union mca_flags
{
	struct
	{
#if __BYTE_ORDER == __LITTLE_ENDIAN
		u_int8_t C :  3;
		u_int8_t N :  5;
		u_int8_t T :  4;
		u_int8_t V :  4;
		u_int8_t Q :  4;
		u_int16_t  : 12; /* reserved */
#else
		u_int16_t  : 12; /* reserved */
		u_int8_t Q :  4;
		u_int8_t V :  4;
		u_int8_t T :  4;
		u_int8_t N :  5;
		u_int8_t C :  3;
#endif
	};
	u_int32_t all;
};

union event_flags
{
	struct
	{
#if __BYTE_ORDER == __LITTLE_ENDIAN
		u_int8_t N  : 1;
		u_int8_t T  : 1;
		u_int8_t PT : 2;
		u_int8_t HT : 2;
		u_int8_t TT : 2;
		u_int8_t CH : 3;
		u_int8_t O  : 1;
		u_int8_t PC : 4;
#else
		u_int8_t PC : 4;
		u_int8_t O  : 1;
		u_int8_t CH : 3;
		u_int8_t TT : 2;
		u_int8_t HT : 2;
		u_int8_t PT : 2;
		u_int8_t T  : 1;
		u_int8_t N  : 1;
#endif
	};
	u_int16_t all;
};

union tick_flags
{
	struct
	{
#if __BYTE_ORDER == __LITTLE_ENDIAN
		u_int8_t N  : 1;
		u_int8_t T  : 1;
		u_int8_t    : 6; /* reserved */
		u_int8_t TL : 1;
		u_int8_t EL : 1;
		u_int8_t MF : 1;
		u_int8_t    : 5; /* reserved */
#else
		u_int8_t    : 5; /* reserved */
		u_int8_t MF : 1;
		u_int8_t EL : 1;
		u_int8_t TL : 1;
		u_int8_t    : 6; /* reserved */
		u_int8_t T  : 1;
		u_int8_t N  : 1;
#endif
	};
	u_int16_t all;
};

union trace_flags
{
	struct {
#if __BYTE_ORDER == __LITTLE_ENDIAN
		u_int8_t OFF : 4;
		u_int8_t TS  : 2;
		u_int8_t TT  : 2;
		u_int8_t STR : 5;
		u_int8_t MP  : 1;
		u_int8_t MH  : 1;
		u_int8_t     : 1; /* reserved */
#else
		u_int8_t     : 1; /* reserved */
		u_int8_t MH  : 1;
		u_int8_t MP  : 1;
		u_int8_t STR : 5;
		u_int8_t TT  : 2;
		u_int8_t TS  : 2;
		u_int8_t OFF : 4;
#endif
	};
	u_int16_t all;
};

/* ------------------------------------------------------------------------- */
/* Let's see if there's a difference in performance */

#ifdef FPGA_USE_MACROS

#define is_mca_hfr(pkt) ( ((struct __fpga_pkt*)pkt)->eth_hdr.ether_type == ETH_MCA_TYPE && \
		((struct __fpga_pkt*)pkt)->fpga_hdr.proto_seq == 0 )
#define get_mca_bin(pkt, bin) ( (u_int32_t) ((struct __fpga_pkt*)pkt)->body[ (bin*BIN_LEN) + \
		( ((struct __fpga_pkt*)pkt)->fpga_hdr.proto_seq ? 0 : MCA_HDR_LEN ) ] )
#define get_mca_flags(pkt) ( (u_int32_t) ((struct __mca_header*) \
		&((struct __fpga_pkt*)pkt)->body)->flags )
#define get_evt_flags(pkt) ( (u_int16_t) ((struct __evt_header*) \
		&((struct __fpga_pkt*)pkt)->body)->flags )
#define get_trace_flags(pkt) ( (u_int16_t) ((struct __trace_header*) \
		&((struct __fpga_pkt*)pkt)->body)->tr_flags )
#define get_evt_toff(pkt) ( (u_int16_t) ((struct __evt_header*) \
		&((struct __fpga_pkt*)pkt)->body)->toff )
/*
 * Convert between event types and linear indices, and back:
 * EVT_TICK_TYPE    0
 * EVT_PEAK_TYPE    1
 * EVT_PLS_TYPE     2
 * EVT_AREA_TYPE    3
 * EVT_TR_SGL_TYPE  4
 * EVT_TR_AVG_TYPE  5
 * EVT_TR_DP_TYPE   6
 * EVT_TR_DPTR_TYPE 7
 * Use it to create and access arrays holding event specific data
 */
#define __is_not_tick(type)       (((type >> 1) & 1) ^ 1)
#define __meas_type_to_idx(type)  ((type >> 2) & 3) /* bits 3 and 4  */
#define __trace_type_to_idx(type) ((type >> 8) & 3) /* bits 9 and 10 */
#define __evt_type_to_idx_2(type) \
	__is_not_tick(type) + \
 	__meas_type_to_idx(type) + \
	__trace_type_to_idx(type)
#define evt_type_to_idx(type)     __evt_type_to_idx_2(type)

/* #define __evt_idx_to_type_2(idx) \ */
	
/* #define evt_idx_to_type(idx)     __evt_idx_to_type_2(idx) */

/* ------------------------------------------------------------------------- */
#else  /* FPGA_USE_MACROS */

static inline int
is_mca_hfr (fpga_pkt* pkt)
{
	return ( pkt->eth_hdr.ether_type == ETH_MCA_TYPE &&
		 pkt->fpga_hdr.proto_seq == 0 );
}

static inline u_int32_t
get_mca_bin (fpga_pkt* pkt, u_int16_t bin)
{
	if (is_mca_hfr (pkt))
		return (u_int32_t) pkt->body[ bin*BIN_LEN + MCA_HDR_LEN ];
	else
		return (u_int32_t) pkt->body[ bin*BIN_LEN ];
}

static inline u_int32_t
get_mca_flags (fpga_pkt* pkt)
{
	return ((struct __mca_header*) &pkt->body)->flags;
}

static inline u_int16_t
get_evt_flags (fpga_pkt* pkt)
{
	return ((struct __evt_header*) &pkt->body)->flags;
}

static inline u_int16_t
get_trace_flags (fpga_pkt* pkt)
{
	return ((struct __trace_header*) &pkt->body)->tr_flags;
}

static inline u_int16_t
get_evt_toff (fpga_pkt* pkt)
{
	return ((struct __evt_header*) &pkt->body)->toff;
}

/*
 * Convert between event types and linear indices, and back:
 * EVT_TICK_TYPE    0
 * EVT_PEAK_TYPE    1
 * EVT_PLS_TYPE     2
 * EVT_AREA_TYPE    3
 * EVT_TR_SGL_TYPE  4
 * EVT_TR_AVG_TYPE  5
 * EVT_TR_DP_TYPE   6
 * EVT_TR_DPTR_TYPE 7
 * Use it to create and access arrays holding event specific data
 */
static inline u_int8_t
evt_type_to_idx (u_int16_t evtype)
{
	return  (((evtype >> 1) & 1) ^ 1) + /* 0 if tick, 1 otherwise */
		((evtype >> 2) & 3) +       /* bits 3 and 4  */
		((evtype >> 8) & 3);        /* bits 9 and 10 */
}

#endif /* FPGA_USE_MACROS */

#ifdef FPGA_DEBUG
#include <assert.h>

static void
__fpga_user_self_test ()
{
	assert (sizeof (union mca_flags) == MCA_FL_LEN);
	assert (sizeof (union event_flags) == EVT_FL_LEN);
	assert (sizeof (union tick_flags) == TICK_FL_LEN);
	assert (sizeof (union trace_flags) == TR_FL_LEN);
	union mca_flags mf = {0,};
	mf.C = 0x07;
	mf.N = 0x1f;
	mf.T = 0x0f;
	mf.V = 0x0f;
	mf.Q = 0x0f;
	assert (mf.all == MCA_FL_MASK);
	union event_flags ef = {0,};
	ef.N  = 0x01;
	ef.T  = 0x01;
	ef.PT = 0x03;
	ef.HT = 0x03;
	ef.TT = 0x03;
	ef.CH = 0x07;
	ef.O  = 0x01;
	ef.PC = 0x0f;
	assert (ef.all == EVT_FL_MASK);
	union tick_flags tf = {0,};
	tf.N  = 0x01;
	tf.T  = 0x01;
	tf.TL = 0x01;
	tf.EL = 0x01;
	tf.MF = 0x01;
	assert (tf.all == TICK_FL_MASK);
	union trace_flags trf = {0,};
	trf.OFF = 0x0f;
	trf.TS  = 0x03;
	trf.TT  = 0x03;
	trf.STR = 0x1f;
	trf.MP  = 0x01;
	trf.MH  = 0x01;
	assert (trf.all == TR_FL_MASK);

	assert (evt_type_to_idx (EVT_TICK_TYPE)    == 0);
	assert (evt_type_to_idx (EVT_PEAK_TYPE)    == 1);
	assert (evt_type_to_idx (EVT_PLS_TYPE)     == 2);
	assert (evt_type_to_idx (EVT_AREA_TYPE)    == 3);
	assert (evt_type_to_idx (EVT_TR_SGL_TYPE)  == 4);
	assert (evt_type_to_idx (EVT_TR_AVG_TYPE)  == 5);
	assert (evt_type_to_idx (EVT_TR_DP_TYPE)   == 6);
	assert (evt_type_to_idx (EVT_TR_DPTR_TYPE) == 7);
	/* assert (evt_idx_to_type (0) ==    EVT_TICK_TYPE); */
	/* assert (evt_idx_to_type (1) ==    EVT_PEAK_TYPE); */
	/* assert (evt_idx_to_type (2) ==     EVT_PLS_TYPE); */
	/* assert (evt_idx_to_type (3) ==    EVT_AREA_TYPE); */
	/* assert (evt_idx_to_type (4) ==  EVT_TR_SGL_TYPE); */
	/* assert (evt_idx_to_type (5) ==  EVT_TR_AVG_TYPE); */
	/* assert (evt_idx_to_type (6) ==   EVT_TR_DP_TYPE); */
	/* assert (evt_idx_to_type (7) == EVT_TR_DPTR_TYPE); */
}

#endif /* FPGA_DEBUG */

#endif