/*
 * Helpers for inspecting ethernet packets of the TES protocol.
 */

#ifndef __NET_TESPKT_GEN_H_INCLUDED__
#define __NET_TESPKT_GEN_H_INCLUDED__

#include "net/tespkt.h"

#if __BYTE_ORDER == TES_BYTE_ORDER
#  define htofs
#  define htofl
#else
#  define htofs bswap16
#  define htofl bswap32
#endif

/* TODO: declarations with descriptions here */

static inline void
tespkt_set_type_mca (tespkt* pkt)
{
	pkt->eth_hdr.ether_type = htons (ETHERTYPE_F_MCA);
}

static inline void
tespkt_set_type_evt (tespkt* pkt)
{
	pkt->eth_hdr.ether_type = htons (ETHERTYPE_F_EVENT);
}

static inline void
tespkt_set_fseq (tespkt* pkt, u_int16_t seq)
{
	pkt->tes_hdr.fseq = htofs (seq);
}

static inline void
tespkt_set_pseq (tespkt* pkt, u_int16_t seq)
{
	pkt->tes_hdr.pseq = htofs (seq);
}

static inline void
tespkt_inc_fseq (tespkt* pkt, u_int16_t seq)
{
	pkt->tes_hdr.fseq = htofs (tespkt_fseq (pkt) + seq);
}

static inline void
tespkt_inc_pseq (tespkt* pkt, u_int16_t seq)
{
	pkt->tes_hdr.pseq = htofs (tespkt_pseq (pkt) + seq);
}

static inline void
tespkt_set_len (tespkt* pkt, u_int16_t len)
{
	pkt->length = htofs (len);
}

static inline void
tespkt_inc_len (tespkt* pkt, u_int16_t len)
{
	pkt->length = htofs (tespkt_flen (pkt) + len);
}

static inline void
tespkt_set_esize (tespkt* pkt, u_int16_t size)
{
	pkt->tes_hdr.esize = htofs (size);
}

static inline void
tespkt_set_etype_tick (tespkt* pkt)
{
	tespkt_set_esize (pkt, 3);
	struct tespkt_event_type* et = tespkt_etype (pkt);
	et->T = 1;
	et->PKT = 0;
	et->TR = 0;
}

static inline void
tespkt_set_etype_nontrace (tespkt* pkt, int pkt_type)
{
	struct tespkt_event_type* et = tespkt_etype (pkt);
	et->T = 0;
	et->PKT = pkt_type;
	et->TR = 0;
}

static inline void
tespkt_set_etype_trace (tespkt* pkt, int tr_type)
{
	if (tr_type != TESPKT_TRACE_TYPE_DP)
		tespkt_set_esize (pkt, 1);
	struct tespkt_event_type* et = tespkt_etype (pkt);
	et->T = 0;
	et->PKT = TESPKT_TYPE_TRACE;
	et->TR = tr_type;
}

#endif
