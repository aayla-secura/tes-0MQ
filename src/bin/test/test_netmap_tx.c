#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <errno.h>
#include <poll.h>
#include <netinet/ether.h>

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>

#define FPGAPKT_DEBUG
// #define FPGA_USE_MACROS
#include <net/fpgapkt.h>

#define DUMP_ROW_LEN   16 /* how many bytes per row when dumping pkt */
#define DUMP_OFF_LEN    5 /* how many digits to use for the offset */
#define UPDATE_INTERVAL 1

#define NM_IFNAME "vale:fpga"
#define MAX_PKTS  1024 /* keep pointers to packets to be freed by the
			* signal handler */


#define MAX_MCA_FRAMES      45
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
 *     IPPROTO_UDP, ETH_ALEN, INADDR_BROADCAST, ETHERTYPE_IP, ETHER_*_LEN
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
		fpga_pkt* slots[MAX_PKTS];
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
fpga_pkt* next_pkt (void)
{
	if (gobj.pkts.last == -1)
		return NULL;

	fpga_pkt* pkt;
	do {
		if (gobj.pkts.cur == gobj.pkts.last)
			gobj.pkts.cur = -1;
		pkt = gobj.pkts.slots[ ++gobj.pkts.cur ];
	} while (pkt == NULL);

	pkt->fpga_hdr.frame_seq = gobj.pkts.sent; /* .sent is incremented
						   * before sending */
	return pkt;
}

static fpga_pkt*
new_fpga_pkt (void)
{
	if (gobj.pkts.first_free == MAX_PKTS)
	{
		ERROR ("Reached maximum number of packets. "
			"Start destroying.\n");
		return NULL;
	}

	fpga_pkt* pkt = malloc (MAX_FPGA_FRAME_LEN);
	if (pkt == NULL)
	{
		errno = ENOMEM;
		raise (SIGTERM);
	}
	memset (pkt, 0, MAX_FPGA_FRAME_LEN);

	struct ether_addr* mac_addr = ether_aton (DST_HW_ADDR);
	memcpy (&pkt->eth_hdr.ether_dhost, mac_addr, ETH_ALEN);
	mac_addr = ether_aton (SRC_HW_ADDR);
	memcpy (&pkt->eth_hdr.ether_shost, mac_addr, ETH_ALEN);
	pkt->fpga_hdr.frame_seq = 0; /* incremented as we send them */
	pkt->length = FPGA_HDR_LEN;

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

static fpga_pkt*
new_mca_pkt (int seq, int num_bins,
			      int num_all_bins,
			      u_int32_t flags)
{
	fpga_pkt* pkt = new_fpga_pkt ();
	if (pkt == NULL)
		return NULL;
	pkt->eth_hdr.ether_type = ETH_MCA_TYPE;
	pkt->length += num_bins * BIN_LEN;

	pkt->fpga_hdr.proto_seq = seq;
	if (seq == 0)
	{
		pkt->length += MCA_HDR_LEN;
		struct mca_header* mh = (struct mca_header*) &pkt->body;
		mh->size = MCA_HDR_LEN + num_all_bins * BIN_LEN;
		mh->last_bin = num_all_bins - 1;
		mh->lowest_value = (u_int32_t) random ();
		/* mh->most_frequent =  */
		mh->flags = flags;
		mh->total = (u_int64_t) mh->lowest_value * num_all_bins;
		mh->start_time = (u_int64_t) random ();
		mh->stop_time = mh->start_time + (u_int32_t) random ();
	}

	return pkt;
}

static fpga_pkt*
new_tick_pkt (u_int16_t flags)
{
	fpga_pkt* pkt = new_fpga_pkt ();
	if (pkt == NULL)
		return NULL;
	pkt->eth_hdr.ether_type = ETH_EVT_TYPE;
	pkt->length += TICK_HDR_LEN;
	pkt->fpga_hdr.evt_size = 3;
	pkt->fpga_hdr.evt_type = EVT_TICK_TYPE;

	struct tick_header* th = (struct tick_header*) &pkt->body;
	th->period = (u_int32_t) random ();
	th->flags = flags;
	th->toff = (u_int16_t) random ();
	th->ts = random ();
	th->ovrfl = (u_int8_t) random ();
	th->err = (u_int8_t) random ();
	th->cfd = (u_int8_t) random ();
	th->lost = (u_int32_t) random ();

	return pkt;
}

static fpga_pkt*
new_peak_pkt (u_int16_t flags)
{
	fpga_pkt* pkt = new_fpga_pkt ();
	if (pkt == NULL)
		return NULL;
	pkt->eth_hdr.ether_type = ETH_EVT_TYPE;
	pkt->length += PEAK_HDR_LEN;
	pkt->fpga_hdr.evt_size = 1;
	pkt->fpga_hdr.evt_type = EVT_PEAK_TYPE;

	struct peak_header* ph = (struct peak_header*) &pkt->body;
	ph->height = (u_int16_t) random ();
	ph->rise_time = (u_int16_t) random ();
	ph->flags = flags;
	ph->toff = (u_int16_t) random ();

	return pkt;
}

static fpga_pkt*
new_pulse_pkt (int num_peaks, u_int16_t flags)
{
	fpga_pkt* pkt = new_fpga_pkt ();
	if (pkt == NULL)
		return NULL;
	pkt->eth_hdr.ether_type = ETH_EVT_TYPE;
	pkt->length += PLS_HDR_LEN + num_peaks * PEAK_LEN;
	pkt->fpga_hdr.evt_size = num_peaks; /* is it?? */
	pkt->fpga_hdr.evt_type = EVT_PLS_TYPE;

	struct pulse_header* ph = (struct pulse_header*) &pkt->body;
	ph->size = (u_int16_t) random ();
	ph->flags = flags;
	ph->toff = (u_int16_t) random ();
	ph->pulse.area = (u_int32_t) random ();
	ph->pulse.length = (u_int16_t) random ();
	ph->pulse.toffset = (u_int16_t) random ();

	/* peaks... */

	return pkt;
}

static fpga_pkt*
new_area_pkt (u_int16_t flags)
{
	fpga_pkt* pkt = new_fpga_pkt ();
	if (pkt == NULL)
		return NULL;
	pkt->eth_hdr.ether_type = ETH_EVT_TYPE;
	pkt->length += AREA_HDR_LEN;
	pkt->fpga_hdr.evt_size = 1;
	pkt->fpga_hdr.evt_type = EVT_AREA_TYPE;

	struct area_header* ah =
		(struct area_header*) &pkt->body;
	ah->area = (u_int32_t) random ();
	ah->flags = flags;
	ah->toff = (u_int16_t) random ();

	return pkt;
}

static fpga_pkt*
new_trace_sgl_pkt (int num_peaks,
				    int num_samples,
				    u_int16_t tr_flags,
				    u_int16_t flags)
{
	fpga_pkt* pkt = new_fpga_pkt ();
	if (pkt == NULL)
		return NULL;
	pkt->eth_hdr.ether_type = ETH_EVT_TYPE;
	pkt->length += TR_FULL_HDR_LEN + num_peaks *
		PEAK_LEN + num_samples * SMPL_LEN;
	pkt->fpga_hdr.evt_size = 1;
	pkt->fpga_hdr.evt_type = EVT_TR_SGL_TYPE;

	struct trace_full_header* th =
		(struct trace_full_header*) &pkt->body;
	th->trace.size = (u_int16_t) random ();
	th->trace.tr_flags = tr_flags;
	th->trace.flags = flags;
	th->trace.toff = (u_int16_t) random ();
	th->pulse.area = (u_int32_t) random ();
	th->pulse.length = (u_int16_t) random ();
	th->pulse.toffset = (u_int16_t) random ();

	/* peaks */

	/* samples */

	return pkt;
}

static fpga_pkt*
new_trace_avg_pkt (int num_samples,
				    u_int16_t tr_flags,
				    u_int16_t flags)
{
	fpga_pkt* pkt = new_fpga_pkt ();
	if (pkt == NULL)
		return NULL;
	pkt->eth_hdr.ether_type = ETH_EVT_TYPE;

	return pkt;
}

static fpga_pkt*
new_trace_dp_pkt (int num_peaks,
				   u_int16_t tr_flags,
				   u_int16_t flags)
{
	fpga_pkt* pkt = new_fpga_pkt ();
	if (pkt == NULL)
		return NULL;
	pkt->eth_hdr.ether_type = ETH_EVT_TYPE;
	pkt->length += TR_FULL_HDR_LEN + num_peaks * PEAK_LEN;
	pkt->fpga_hdr.evt_size = 1;
	pkt->fpga_hdr.evt_type = EVT_TR_DP_TYPE;

	struct trace_full_header* th =
		(struct trace_full_header*) &pkt->body;
	th->trace.size = (u_int16_t) random ();
	th->trace.tr_flags = tr_flags;
	th->trace.flags = flags;
	th->trace.toff = (u_int16_t) random ();
	th->pulse.area = (u_int32_t) random ();
	th->pulse.length = (u_int16_t) random ();
	th->pulse.toffset = (u_int16_t) random ();

	/* peaks */

	struct dot_prod* dp = (struct dot_prod*)(
		(u_char*) pkt + pkt->length );
	/* Don't know how to cast to 48-bit integer and don't want to write to
	 * reserved bits, so write 64-bits to a temporary struct and copy only
	 * the used field */
	struct dot_prod dptmp = {0,};
	u_int64_t rand = random ();
	memcpy (&dptmp, &rand, DP_LEN);
	dp->dot_prod = dptmp.dot_prod;
	pkt->length += DP_LEN;

	return pkt;
}

static fpga_pkt*
new_trace_dptr_pkt (int num_peaks,
				     int num_samples,
				     u_int16_t tr_flags,
				     u_int16_t flags)
{
	fpga_pkt* pkt = new_fpga_pkt ();
	if (pkt == NULL)
		return NULL;
	pkt->eth_hdr.ether_type = ETH_EVT_TYPE;

	return pkt;
}

static void
destroy_pkt (int id)
{
	assert (id >= 0);
	assert (id <= gobj.pkts.last);
	fpga_pkt* pkt = gobj.pkts.slots[id];
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
dump_pkt (fpga_pkt* _pkt)
{
	u_int16_t len = _pkt->length;
	const char* pkt = (const char*)_pkt;
	char buf[ 4*DUMP_ROW_LEN + DUMP_OFF_LEN + 2 + 1 ];

	memset (buf, 0, sizeof (buf));
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
	puts ("");
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
	// fpgapkt_self_test ();

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

	/* Create some packets before starting and only increment frame_seq
	 * while sending */
	do
	{
		fpga_pkt* pkt; /* for checking is max is reached */

		/* --------------- A full MCA histogram --------------- */
		/* the header frame */
		pkt = new_mca_pkt (0, MAX_MCA_BINS_HFR, MAX_MCA_BINS_ALL, 0);
		if (pkt == NULL)
			break; /* Reached max */
		dump_pkt (pkt);
		assert (pkt_len (pkt) <= MAX_FPGA_FRAME_LEN);
		/* the rest of the frames */
		for (int f = 1; f < MAX_MCA_FRAMES; f++)
		{
			fpga_pkt* pkt = new_mca_pkt (f, MAX_MCA_BINS_SFR,
				MAX_MCA_BINS_ALL, 0);
			if (pkt == NULL)
				break; /* Reached max */
			dump_pkt (pkt);
			assert (pkt_len (pkt) <= MAX_FPGA_FRAME_LEN);
		}

		/* ---------------- Some event packets ---------------- */
		pkt = new_tick_pkt (0);
		if (pkt == NULL)
			break; /* Reached max */
		dump_pkt (pkt);
		assert (pkt_len (pkt) <= MAX_FPGA_FRAME_LEN);
		pkt = new_peak_pkt (0);
		if (pkt == NULL)
			break; /* Reached max */
		dump_pkt (pkt); // FIX
		assert (pkt_len (pkt) <= MAX_FPGA_FRAME_LEN);
		pkt = new_pulse_pkt (MAX_PLS_PEAKS, 0);
		if (pkt == NULL)
			break; /* Reached max */
		dump_pkt (pkt);
		assert (pkt_len (pkt) <= MAX_FPGA_FRAME_LEN);
		pkt = new_area_pkt (0);
		if (pkt == NULL)
			break; /* Reached max */
		dump_pkt (pkt); // FIX
		assert (pkt_len (pkt) <= MAX_FPGA_FRAME_LEN);
		pkt = new_trace_sgl_pkt (MAX_TR_SGL_PEAKS_HFR / 2,
			MAX_TR_SGL_SMPLS_HFR / 2, 0, 0);
		if (pkt == NULL)
			break; /* Reached max */
		dump_pkt (pkt); // FIX
		assert (pkt_len (pkt) <= MAX_FPGA_FRAME_LEN);
		pkt = new_trace_dp_pkt (MAX_TR_DP_PEAKS_HFR, 0, 0);
		if (pkt == NULL)
			break; /* Reached max */
		dump_pkt (pkt);
		assert (pkt_len (pkt) <= MAX_FPGA_FRAME_LEN);
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
			fpga_pkt* pkt = next_pkt ();
			assert (pkt);

			struct netmap_slot* cur_slot =
				&txring->slot[ txring->cur ];
			nm_pkt_copy (pkt,
				     NETMAP_BUF (txring, cur_slot->buf_idx),
				     pkt->length);
		
			cur_slot->len = pkt->length;
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
