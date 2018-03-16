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

#endif
