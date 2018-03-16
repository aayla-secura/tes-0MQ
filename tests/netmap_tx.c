#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <ctype.h> // isprint
#include <sys/types.h>
#include <sys/time.h>
#include <errno.h>
#include <poll.h>
#include <net/ethernet.h>
#ifdef linux
#  include <netinet/ether.h>
#endif

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>

#define TESPKT_DEBUG
#include "net/tespkt_gen.h"

#define DUMP_ROW_LEN   16 /* how many bytes per row when dumping pkt */
#define DUMP_OFF_LEN    5 /* how many digits to use for the offset */
#define UPDATE_INTERVAL 1

#define NM_IFNAME "vale0:vi0"
#define MAX_PKTS  1024 /* keep pointers to packets to be freed by the
                        * signal handler */

#define EVT_TYPE_LEN  2
#define MCA_FL_LEN    4
#define EVT_FL_LEN    2
#define TICK_FL_LEN   2
#define TRACE_FL_LEN  2

#define DP_LEN                 8
#define SMPL_LEN               2
#define MAX_MCA_FRAMES        45
// #define MAX_TRACE_FRAMES
#define MAX_MCA_BINS_ALL      ((65528 - TESPKT_MCA_HDR_LEN) / TESPKT_MCA_BIN_LEN)
#define MAX_MCA_BINS_HFR      ((TESPKT_MTU - TESPKT_MCA_HDR_LEN - \
				TESPKT_HDR_LEN) / TESPKT_MCA_BIN_LEN)
#define MAX_MCA_BINS_SFR      ((TESPKT_MTU - \
				TESPKT_HDR_LEN) / TESPKT_MCA_BIN_LEN)
#define MAX_PLS_PEAKS         ((TESPKT_MTU - TESPKT_HDR_LEN - \
				TESPKT_PULSE_HDR_LEN) / TESPKT_PEAK_LEN)
#define MAX_TR_SGL_PEAKS_HFR  ((TESPKT_MTU - TESPKT_HDR_LEN - \
				TESPKT_TRACE_FULL_HDR_LEN) / TESPKT_PEAK_LEN)
#define MAX_TR_SGL_SMPLS_HFR  ((TESPKT_MTU - TESPKT_HDR_LEN - \
				TESPKT_TRACE_FULL_HDR_LEN) / SMPL_LEN)
#define MAX_TR_AVG_SMPLS_HFR  ((TESPKT_MTU - TESPKT_HDR_LEN - \
				TESPKT_TRACE_HDR_LEN) / SMPL_LEN)
#define MAX_TR_DP_PEAKS_HFR   ((TESPKT_MTU - TESPKT_HDR_LEN - \
				TESPKT_TRACE_FULL_HDR_LEN - DP_LEN) / TESPKT_PEAK_LEN)
#define MAX_TR_DPTR_PEAKS_HFR ((TESPKT_MTU - TESPKT_HDR_LEN - \
				TESPKT_TRACE_FULL_HDR_LEN - DP_LEN) / TESPKT_PEAK_LEN)
#define MAX_TR_DPTR_SMPLS_HFR ((TESPKT_MTU - TESPKT_HDR_LEN - \
				TESPKT_TRACE_FULL_HDR_LEN - DP_LEN) / SMPL_LEN)

#define SRC_HW_ADDR "ff:ff:ff:ff:ff:ff"
#define DST_HW_ADDR "ff:ff:ff:ff:ff:ff"

#define ERROR(...) fprintf (stdout, __VA_ARGS__)
#define DEBUG(...) fprintf (stderr, __VA_ARGS__)
#define INFO(...)  fprintf (stdout, __VA_ARGS__)

/*
 * USEFUL:
 *   from system headers:
 *     struct ether_header, struct ether_addr
 *     ether_aton_r, ether_ntoa_r, ntohl, ntohs, htonl, htons
 *     inet_aton, inet_ntoa, inet_addr, inet_network, inet_pton, inet_ntop
 *     getifaddrs
 *     IPPROTO_UDP, ETHER_ADDR_LEN, INADDR_BROADCAST, ETHERTYPE_IP, ETHER_*_LEN
 * 
 *   from pkt-gen:
 *     system_ncpus, dump_payload, source_hwaddr, checksum
 * 
 * TO DO:
 *   check if passing pointer to flag union is faster than passing it as int
 */

static struct
{
	struct nm_desc* nmd;
	struct {
		struct timeval start;
		struct timeval last_check;
	} timers;
	struct {
		tespkt* slots[MAX_PKTS];
		int last;       /* highest allocated index */

		int first_free; /* lowest unallocated index */
		int cur;        /* used by next_pkt to walk through all created
		                 * in a circular fashion */
		u_int32_t last_sent;
		u_int32_t sent;
	} pkts;
	u_int32_t loop;
} gobj = { .pkts.last = -1, .pkts.cur = -1 };

static inline
tespkt* next_pkt (void)
{
	if (gobj.pkts.last == -1)
		return NULL;

	tespkt* pkt;
	do {
		if (gobj.pkts.cur == gobj.pkts.last)
			gobj.pkts.cur = -1;
		pkt = gobj.pkts.slots[ ++gobj.pkts.cur ];
	} while (pkt == NULL);

	tespkt_set_fseq (pkt, gobj.pkts.sent); /* .sent is incremented
	                                 * before sending */
	return pkt;
}

static tespkt*
new_tespkt (void)
{
	if (gobj.pkts.first_free == MAX_PKTS)
	{
		ERROR ("Reached maximum number of packets. "
			"Start destroying.\n");
		return NULL;
	}

	tespkt* pkt = (tespkt*) malloc (TESPKT_MTU);
	if (pkt == NULL)
		raise (SIGTERM);
	memset (pkt, 0, sizeof (tespkt));

	struct ether_addr* mac_addr = ether_aton (DST_HW_ADDR);
	memcpy (&pkt->eth_hdr.ether_dhost, mac_addr, ETHER_ADDR_LEN);
	mac_addr = ether_aton (SRC_HW_ADDR);
	memcpy (&pkt->eth_hdr.ether_shost, mac_addr, ETHER_ADDR_LEN);
	tespkt_set_fseq (pkt, 0); /* incremented as we send them */
	tespkt_set_len (pkt, TESPKT_HDR_LEN); /* incremented later */

	/* store a global pointer */
	gobj.pkts.slots[gobj.pkts.first_free] = pkt;
	assert (gobj.pkts.first_free != gobj.pkts.last);
	DEBUG ("Creating packet #%d\n", gobj.pkts.first_free);
	if (gobj.pkts.first_free < gobj.pkts.last)
	{
		/* there were holes in the array, find the next freed slot */
		int p;
		for (p = gobj.pkts.first_free + 1; p <= gobj.pkts.last; p++)
		{
			if (gobj.pkts.slots[p] == NULL)
				break;
		}
		gobj.pkts.first_free = p;
	}
	else
	{
		/* simply append */
		gobj.pkts.first_free++;
		gobj.pkts.last++;
	}

	return pkt;
}

static tespkt*
new_mca_pkt (int seq, int nbins,
			  int num_all_bins,
			  struct tespkt_mca_flags* flags)
{
	tespkt* pkt = new_tespkt ();
	if (pkt == NULL)
		return NULL;
	tespkt_set_type_mca (pkt);
	tespkt_inc_len (pkt, nbins * TESPKT_MCA_BIN_LEN);

	tespkt_set_pseq (pkt, seq);
	if (seq == 0)
	{
		tespkt_inc_len (pkt, TESPKT_MCA_HDR_LEN);
		struct tespkt_mca_hdr* mh = (struct tespkt_mca_hdr*) &pkt->body;
		mh->size = TESPKT_MCA_HDR_LEN + num_all_bins * TESPKT_MCA_BIN_LEN;
		mh->last_bin = num_all_bins - 1;
		mh->lowest_value = (u_int32_t) random ();
		/* mh->most_frequent =  */
		if (flags != NULL)
			memcpy(&mh->flags, flags, MCA_FL_LEN);
		mh->total = (u_int64_t) mh->lowest_value * num_all_bins;
		mh->start_time = (u_int64_t) random ();
		mh->stop_time = mh->start_time + (u_int32_t) random ();
	}

	return pkt;
}

static tespkt*
new_tick_pkt (struct tespkt_tick_flags* flags)
{
	tespkt* pkt = new_tespkt ();
	if (pkt == NULL)
		return NULL;
	tespkt_set_type_evt (pkt);
	tespkt_inc_len (pkt, TESPKT_TICK_HDR_LEN);
	tespkt_set_esize (pkt, 3);
	struct tespkt_event_type* et = tespkt_etype (pkt);
	et->T = 1;

	struct tespkt_tick_hdr* th = (struct tespkt_tick_hdr*) &pkt->body;
	th->period = (u_int32_t) random ();
	if (flags != NULL)
		memcpy(&th->flags, flags, TICK_FL_LEN);
	th->toff = (u_int16_t) random ();
	th->ts = random ();
	th->ovrfl = (u_int8_t) random ();
	th->err = (u_int8_t) random ();
	th->cfd = (u_int8_t) random ();
	th->lost = (u_int32_t) random ();

	return pkt;
}

static tespkt*
new_peak_pkt (struct tespkt_event_flags* flags)
{
	tespkt* pkt = new_tespkt ();
	if (pkt == NULL)
		return NULL;
	tespkt_set_type_evt (pkt);
	tespkt_inc_len (pkt, TESPKT_PEAK_HDR_LEN);
	tespkt_set_esize (pkt, 1);
	struct tespkt_event_type* et = tespkt_etype (pkt);
	et->T = 0;
	et->PKT = TESPKT_TYPE_PEAK;

	struct tespkt_peak_hdr* ph = (struct tespkt_peak_hdr*) &pkt->body;
	ph->height = (u_int16_t) random ();
	ph->rise_time = (u_int16_t) random ();
	if (flags != NULL)
		memcpy(&ph->flags, flags, EVT_FL_LEN);
	ph->toff = (u_int16_t) random ();

	return pkt;
}

static tespkt*
new_area_pkt (struct tespkt_event_flags* flags)
{
	tespkt* pkt = new_tespkt ();
	if (pkt == NULL)
		return NULL;
	tespkt_set_type_evt (pkt);
	tespkt_inc_len (pkt, TESPKT_AREA_HDR_LEN);
	tespkt_set_esize (pkt, 1);
	struct tespkt_event_type* et = tespkt_etype (pkt);
	et->T = 0;
	et->PKT = TESPKT_TYPE_AREA;

	struct tespkt_area_hdr* ah =
		(struct tespkt_area_hdr*) &pkt->body;
	ah->area = (u_int32_t) random ();
	if (flags != NULL)
		memcpy(&ah->flags, flags, EVT_FL_LEN);
	ah->toff = (u_int16_t) random ();

	return pkt;
}

static tespkt*
new_pulse_pkt (int num_peaks, struct tespkt_event_flags* flags)
{
	tespkt* pkt = new_tespkt ();
	if (pkt == NULL)
		return NULL;
	tespkt_set_type_evt (pkt);
	tespkt_inc_len (pkt,
		TESPKT_PULSE_HDR_LEN + num_peaks * TESPKT_PEAK_LEN);
	tespkt_set_esize (pkt, 1);
	struct tespkt_event_type* et = tespkt_etype (pkt);
	et->T = 0;
	et->PKT = TESPKT_TYPE_PULSE;

	struct tespkt_pulse_hdr* ph = (struct tespkt_pulse_hdr*) &pkt->body;
	ph->size = (u_int16_t) random ();
	if (flags != NULL)
		memcpy(&ph->flags, flags, EVT_FL_LEN);
	ph->toff = (u_int16_t) random ();
	ph->pulse.area = (u_int32_t) random ();
	ph->pulse.length = (u_int16_t) random ();
	ph->pulse.toffset = (u_int16_t) random ();

	/* peaks... */

	return pkt;
}

static tespkt*
new_trace_sgl_pkt (int num_peaks,
			int num_samples,
			struct tespkt_trace_flags* tr_flags,
			struct tespkt_event_flags* flags)
{
	tespkt* pkt = new_tespkt ();
	if (pkt == NULL)
		return NULL;
	tespkt_set_type_evt (pkt);
	tespkt_inc_len (pkt, TESPKT_TRACE_FULL_HDR_LEN + num_peaks *
		TESPKT_PEAK_LEN + num_samples * SMPL_LEN);
	tespkt_set_esize (pkt, 1);
	struct tespkt_event_type* et = tespkt_etype (pkt);
	et->T = 0;
	et->PKT = TESPKT_TYPE_TRACE;
	et->TR = TESPKT_TRACE_TYPE_SGL;

	struct tespkt_trace_full_hdr* th =
		(struct tespkt_trace_full_hdr*) &pkt->body;
	th->trace.size = (u_int16_t) random ();
	if (tr_flags != NULL)
		memcpy(&th->trace.tr_flags, tr_flags, TRACE_FL_LEN);
	if (flags != NULL)
		memcpy(&th->trace.flags, flags, EVT_FL_LEN);
	th->trace.toff = (u_int16_t) random ();
	th->pulse.area = (u_int32_t) random ();
	th->pulse.length = (u_int16_t) random ();
	th->pulse.toffset = (u_int16_t) random ();

	/* peaks */

	/* samples */

	return pkt;
}

static tespkt*
new_trace_avg_pkt (int num_samples,
			struct tespkt_trace_flags* tr_flags,
			struct tespkt_event_flags* flags)
{
	tespkt* pkt = new_tespkt ();
	if (pkt == NULL)
		return NULL;
	tespkt_set_type_evt (pkt);

	return pkt;
}

static tespkt*
new_trace_dp_pkt (int num_peaks,
		   struct tespkt_trace_flags* tr_flags,
		   struct tespkt_event_flags* flags)
{
	tespkt* pkt = new_tespkt ();
	if (pkt == NULL)
		return NULL;
	tespkt_set_type_evt (pkt);
	tespkt_inc_len (pkt, TESPKT_TRACE_FULL_HDR_LEN + num_peaks * TESPKT_PEAK_LEN);
	tespkt_set_esize (pkt, 1);
	struct tespkt_event_type* et = tespkt_etype (pkt);
	et->T = 0;
	et->PKT = TESPKT_TYPE_TRACE;
	et->TR = TESPKT_TRACE_TYPE_DP;

	struct tespkt_trace_full_hdr* th =
		(struct tespkt_trace_full_hdr*) &pkt->body;
	th->trace.size = (u_int16_t) random ();
	if (tr_flags != NULL)
		memcpy(&th->trace.tr_flags, tr_flags, TRACE_FL_LEN);
	if (flags != NULL)
		memcpy(&th->trace.flags, flags, EVT_FL_LEN);
	th->trace.toff = (u_int16_t) random ();
	th->pulse.area = (u_int32_t) random ();
	th->pulse.length = (u_int16_t) random ();
	th->pulse.toffset = (u_int16_t) random ();

	/* peaks */

	struct tespkt_dot_prod* dp = (struct tespkt_dot_prod*)(
		(u_char*) pkt + tespkt_flen (pkt) );
	/* Don't know how to cast to 48-bit integer and don't want to write to
	 * reserved bits, so write 64-bits to a temporary struct and copy only
	 * the used field */
	struct tespkt_dot_prod dptmp = {0};
	u_int64_t rand = random ();
	memcpy (&dptmp, &rand, DP_LEN);
	dp->dot_prod = dptmp.dot_prod;
	tespkt_inc_len (pkt, DP_LEN);

	return pkt;
}

static tespkt*
new_trace_dptr_pkt (int num_peaks,
			 int num_samples,
			 struct tespkt_trace_flags* tr_flags,
			 struct tespkt_event_flags* flags)
{
	tespkt* pkt = new_tespkt ();
	if (pkt == NULL)
		return NULL;
	tespkt_set_type_evt (pkt);

	return pkt;
}

static void
destroy_pkt (int id)
{
	assert (id >= 0);
	assert (id <= gobj.pkts.last);
	tespkt* pkt = gobj.pkts.slots[id];
	if (pkt)
	{
		DEBUG ("Destroying packet #%d\n", id);
		free (pkt);
		gobj.pkts.slots[id] = NULL;
		if (id == gobj.pkts.last)
			gobj.pkts.last--;
		if (id < gobj.pkts.first_free)
			gobj.pkts.first_free = id;

	}
}

static void
dump_pkt (tespkt* _pkt)
{
	u_int16_t len = tespkt_flen (_pkt);
	const char* pkt = (const char*)_pkt;
	char buf[ 4*DUMP_ROW_LEN + DUMP_OFF_LEN + 2 + 1 ] = {0};

	for (int r = 0; r < len; r += DUMP_ROW_LEN) {
		sprintf (buf, "%0*x: ", DUMP_OFF_LEN, r);

		/* hexdump */
		for (int b = 0; b < DUMP_ROW_LEN && b+r < len; b++)
			sprintf (buf + DUMP_OFF_LEN + 2 + 3*b, "%02x ",
				(u_int8_t)(pkt[b+r]));

		/* ASCII dump */
		for (int b = 0; b < DUMP_ROW_LEN && b+r < len; b++)
			sprintf (buf + DUMP_OFF_LEN + 2 + b + 3*DUMP_ROW_LEN,
				"%c", isprint (pkt[b+r]) ? pkt[b+r] : '.');

		DEBUG ("%s\n", buf);
	}
	DEBUG ("\n");
}

static void
print_desc_info (void)
{
	INFO (
		"ringid: %hu, flags: %u, cmd: %hu\n"
		"extra rings: %hu, extra buffers: %u\n"
		"done_mmap: %d\n"
		"rx rings: %hu, rx slots: %u\n"
		"tx rings: %hu, tx slots: %u\n"
		"first rx: %hu, last rx: %hu\n"
		"first tx: %hu, last tx: %hu\n"
		"snaplen: %d\npromisc: %d\n",
		gobj.nmd->req.nr_ringid,
		gobj.nmd->req.nr_flags,
		gobj.nmd->req.nr_cmd,
		gobj.nmd->req.nr_arg1,
		gobj.nmd->req.nr_arg3,
		gobj.nmd->done_mmap,
		gobj.nmd->req.nr_rx_rings,
		gobj.nmd->req.nr_rx_slots,
		gobj.nmd->req.nr_tx_rings,
		gobj.nmd->req.nr_tx_slots,
		gobj.nmd->first_rx_ring,
		gobj.nmd->last_rx_ring,
		gobj.nmd->first_tx_ring,
		gobj.nmd->last_tx_ring,
		gobj.nmd->snaplen,
		gobj.nmd->promisc
		);
}

static void
print_stats (int sig)
{
	if ( ! timerisset (&gobj.timers.start) )
		return;

	struct timeval* tprev = &gobj.timers.last_check;
	if ( ( ! timerisset (tprev) ) || sig == 0 )
		tprev = &gobj.timers.start;

	struct timeval tnow, tdiff;
	gettimeofday (&tnow, NULL);

	timersub (&tnow, tprev, &tdiff);
	double tdelta = tdiff.tv_sec + 1e-6 * tdiff.tv_usec;
	
	if (sig)
	{
		assert (sig == SIGALRM);
		/* Alarm went off, update stats */
		u_int32_t new_sent = gobj.pkts.sent - gobj.pkts.last_sent;
		INFO (
			"total pkts sent: %10u ; "
			/* "new pkts sent: %10u ; " */
			"avg bandwidth: %10.3e pps\n",
			gobj.pkts.sent,
			/* new_sent, */
			(double) new_sent / tdelta
			);

		memcpy (&gobj.timers.last_check, &tnow,
			sizeof (struct timeval));
		gobj.pkts.last_sent = gobj.pkts.sent;

		/* Set alarm again */
		alarm (UPDATE_INTERVAL);
	}
	else
	{
		/* Called by cleanup, print final stats */
		INFO (
			"\n-----------------------------\n"
			"looped:            %10u\n"
			"packets sent:      %10u\n"
			"avg pkts per loop: %10u\n"
			"avg bandwidth:     %10.3e pps\n"
			"-----------------------------\n",
			gobj.loop,
			gobj.pkts.sent,
			(gobj.loop > 0) ? gobj.pkts.sent / gobj.loop : 0,
			(double) gobj.pkts.sent / tdelta
			);
	}
}

static void
cleanup (int sig)
{
	if (sig == SIGINT)
		INFO ("Interrupted\n");

	int rc = EXIT_SUCCESS;
	if (errno)
	{
		perror ("");
		rc = EXIT_FAILURE;
	}

	if (gobj.nmd)
	{
		print_stats (0);
		nm_close (gobj.nmd);
	}

	for (int p = 0; p <= gobj.pkts.last; p++)
		destroy_pkt (p);

	exit (rc);
}

int
main (void)
{
	/* Debugging */
	tespkt_self_test ();

	srandom ( (unsigned int) random () );
	int rc;

	/* Signal handlers */
	struct sigaction sigact;

	sigact.sa_flags = 0;
	sigact.sa_handler = cleanup;
	sigemptyset (&sigact.sa_mask);
	sigaddset (&sigact.sa_mask, SIGINT);
	sigaddset (&sigact.sa_mask, SIGTERM);
	sigaddset (&sigact.sa_mask, SIGALRM);
	rc = sigaction (SIGINT, &sigact, NULL);
	rc |= sigaction (SIGTERM, &sigact, NULL);

	sigact.sa_handler = print_stats;
	sigemptyset (&sigact.sa_mask);
	rc |= sigaction (SIGALRM, &sigact, NULL);

	if (rc == -1)
	{
		perror ("sigaction");
		exit (EXIT_FAILURE);
	}

	/* Open the interface */
	gobj.nmd = nm_open (NM_IFNAME"{1", NULL, 0, 0);
	if (gobj.nmd == NULL)
	{
		ERROR ("Could not open interface %s\n", NM_IFNAME);
		exit (EXIT_FAILURE);
	}
	print_desc_info ();

	/* ------------------------------------------------------------ */

	/* Create some packets before starting and only increment tespkt_fseq
	 * while sending */
	do
	{
		tespkt* pkt; /* for checking is max is reached */
		int err;
		// pkt = new_tespkt ();
		// break;

		/* --------------- A full MCA histogram --------------- */
		/* the header frame */
		int nbins_left = MAX_MCA_BINS_ALL;
		pkt = new_mca_pkt (0, MAX_MCA_BINS_HFR, MAX_MCA_BINS_ALL, NULL);
		if (pkt == NULL)
			break; /* Reached max */
		err = tespkt_is_valid (pkt);
		if (err)
		{
			tespkt_perror (stdout, err);
			raise (SIGTERM);
		}
		dump_pkt (pkt);
		assert (tespkt_flen (pkt) <= TESPKT_MTU);
		nbins_left -= MAX_MCA_BINS_HFR;
		/* the rest of the frames */
		for (int f = 1; ; f++)
		{
			assert (f < MAX_MCA_FRAMES);
			if (nbins_left > MAX_MCA_BINS_SFR)
			{
				pkt = new_mca_pkt (f,
					MAX_MCA_BINS_SFR, MAX_MCA_BINS_ALL, NULL);
			}
			else
			{
				pkt = new_mca_pkt (f,
					nbins_left, MAX_MCA_BINS_ALL, NULL);
				break;
			}
			if (pkt == NULL)
				break; /* Reached max */
			err = tespkt_is_valid (pkt);
			if (err)
			{
				tespkt_perror (stdout, err);
				raise (SIGTERM);
			}
			dump_pkt (pkt);
			assert (tespkt_flen (pkt) <= TESPKT_MTU);
			nbins_left -= MAX_MCA_BINS_SFR;
		}

		/* ---------------- Some event packets ---------------- */
		pkt = new_tick_pkt (NULL);
		if (pkt == NULL)
			break; /* Reached max */
		err = tespkt_is_valid (pkt);
		if (err)
		{
			tespkt_perror (stdout, err);
			raise (SIGTERM);
		}
		dump_pkt (pkt);
		assert (tespkt_flen (pkt) <= TESPKT_MTU);
		pkt = new_peak_pkt (NULL);
		if (pkt == NULL)
			break; /* Reached max */
		err = tespkt_is_valid (pkt);
		if (err)
		{
			tespkt_perror (stdout, err);
			raise (SIGTERM);
		}
		dump_pkt (pkt);
		assert (tespkt_flen (pkt) <= TESPKT_MTU);
		pkt = new_pulse_pkt (MAX_PLS_PEAKS, NULL);
		if (pkt == NULL)
			break; /* Reached max */
		err = tespkt_is_valid (pkt);
		if (err)
		{
			tespkt_perror (stdout, err);
			raise (SIGTERM);
		}
		dump_pkt (pkt);
		assert (tespkt_flen (pkt) <= TESPKT_MTU);
		pkt = new_area_pkt (NULL);
		if (pkt == NULL)
			break; /* Reached max */
		err = tespkt_is_valid (pkt);
		if (err)
		{
			tespkt_perror (stdout, err);
			raise (SIGTERM);
		}
		dump_pkt (pkt);
		assert (tespkt_flen (pkt) <= TESPKT_MTU);
		pkt = new_trace_sgl_pkt (MAX_TR_SGL_PEAKS_HFR / 2,
			MAX_TR_SGL_SMPLS_HFR / 2, NULL, NULL);
		if (pkt == NULL)
			break; /* Reached max */
		err = tespkt_is_valid (pkt);
		if (err)
		{
			tespkt_perror (stdout, err);
			raise (SIGTERM);
		}
		dump_pkt (pkt);
		assert (tespkt_flen (pkt) <= TESPKT_MTU);
		pkt = new_trace_dp_pkt (MAX_TR_DP_PEAKS_HFR, NULL, NULL);
		if (pkt == NULL)
			break; /* Reached max */
		err = tespkt_is_valid (pkt);
		if (err)
		{
			tespkt_perror (stdout, err);
			raise (SIGTERM);
		}
		dump_pkt (pkt);
		assert (tespkt_flen (pkt) <= TESPKT_MTU);
	} while (0);

	/* Get the ring (we only use one) */
	assert (gobj.nmd->first_tx_ring == gobj.nmd->last_tx_ring);
	struct netmap_ring* txring = NETMAP_TXRING (
		gobj.nmd->nifp, gobj.nmd->cur_tx_ring);

	/* Start the clock */
	rc = gettimeofday (&gobj.timers.start, NULL);
	if (rc == -1)
		raise (SIGTERM);

	/* Set the alarm */
	alarm (UPDATE_INTERVAL);

	/* Poll */
	struct pollfd pfd;
	pfd.fd = gobj.nmd->fd;
	pfd.events = POLLOUT;
	INFO ("\nStarting poll\n");

	for (gobj.loop = 1, errno = 0 ;; gobj.loop++)
	{
		rc = poll (&pfd, 1, 1000);
		if (rc == -1 && errno == EINTR)
			errno = 0;
		else if (rc == -1)
			raise (SIGTERM);
		else if (rc == 0)
		{
			DEBUG ("poll timed out\n");
			continue;
		}

		while ( ! nm_ring_empty (txring) )
		{
			tespkt* pkt = next_pkt ();
			assert (pkt);

			struct netmap_slot* cur_slot =
				&txring->slot[ txring->cur ];
			nm_pkt_copy (pkt,
				 NETMAP_BUF (txring, cur_slot->buf_idx),
					 tespkt_flen (pkt));
		
			cur_slot->len = tespkt_flen (pkt);
			txring->head = txring->cur =
				nm_ring_next (txring, txring->cur);
			gobj.pkts.sent++;
			if (gobj.pkts.sent + 1 == 0)
			{
				errno = EOVERFLOW;
				raise (SIGINT);
			}
		}
	}

	errno = 0;
	raise (SIGTERM); /*cleanup*/
	return 0; /*never reached, suppress gcc warning*/
}
