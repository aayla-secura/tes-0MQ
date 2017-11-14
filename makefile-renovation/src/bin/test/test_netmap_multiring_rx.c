#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <errno.h>
#include <assert.h>
#include <poll.h>
#include <net/ethernet.h>
#ifdef linux
# include <netinet/ether.h>
#endif
#include <net/if.h> /* IFNAMSIZ */

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>

#define FPGAPKT_DEBUG
#include <net/fpgapkt.h>

#ifndef NMRING
#define NMRING
#endif
#define MAX_RINGS 24
#define UPDATE_INTERVAL 1
#ifndef NMIF
#define NMIF "vale0:vi1"
#endif

#ifndef QUIET
#define VERBOSE
#endif

#define ERROR(...) fprintf (stdout, __VA_ARGS__)
#define DEBUG(...) fprintf (stderr, __VA_ARGS__)
#define INFO(...)  fprintf (stdout, __VA_ARGS__)

static struct
{
	struct nm_desc* nmd;
	struct {
		struct timeval start;
		struct timeval last_check;
	} timers;
	struct {
		u_int32_t last_rcvd;
		u_int32_t rcvd;
		u_int32_t missed;
		u_int16_t last_id;
		u_int32_t inslot[MAX_RINGS];
	} pkts;
	u_int32_t loop;
	u_int8_t skipped;
} gobj;

static void
print_desc_info (void)
{
	INFO (
		"name: %s\n"
		"ringid: %hu, flags: %u, cmd: %hu\n"
		"extra rings: %hu, extra buffers: %u\n"
		"done_mmap: %d\n"
		"rx rings: %hu, rx slots: %u\n"
		"tx rings: %hu, tx slots: %u\n"
		"first rx: %hu, last rx: %hu\n"
		"first tx: %hu, last tx: %hu\n"
		"snaplen: %d\npromisc: %d\n",
		gobj.nmd->nifp->ni_name,
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
		u_int32_t new_rcvd = gobj.pkts.rcvd - gobj.pkts.last_rcvd;
		INFO (
			"total pkts received: %10u ; "
			"total pkts missed: %10u ; "
			/* "new pkts received: %10u ; " */
			"avg bandwidth: %10.3e pps\n",
			gobj.pkts.rcvd,
			gobj.pkts.missed,
			/* new_rcvd, */
			(double) new_rcvd / tdelta
			);

		memcpy (&gobj.timers.last_check, &tnow,
			sizeof (struct timeval));
		gobj.pkts.last_rcvd = gobj.pkts.rcvd;

		/* Set alarm again */
		alarm (UPDATE_INTERVAL);
	}
	else
	{
		/* Called by cleanup, print final stats */
		INFO (
			"\n-----------------------------\n"
			"looped:            %10u\n"
			"packets received:  %10u / %u\n"
			"packets missed:    %10u\n"
			"avg pkts per loop: %10u\n"
			"avg bandwidth:     %10.3e pps\n"
			"-----------------------------\n",
			gobj.loop,
			gobj.pkts.rcvd,
			gobj.pkts.rcvd + gobj.pkts.missed,
			gobj.pkts.missed,
			(gobj.loop > 0) ? gobj.pkts.rcvd / gobj.loop : 0,
			(double) gobj.pkts.rcvd / tdelta
			);
		for (int s = 0; s <= gobj.nmd->last_rx_ring - gobj.nmd->first_rx_ring; s++)
		{
			INFO (
				"slot %d received:  %10u\n",
				s, gobj.pkts.inslot[s]
			);
		}
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

	if (gobj.nmd != NULL)
	{
		print_stats (0);
		nm_close (gobj.nmd);
	}

	exit (rc);
}

static void
rx_handler (u_char* arg, const struct nm_pkthdr* hdr, const u_char* buf)
{
	fpga_pkt* pkt = (fpga_pkt*)buf;
	uint16_t ri = gobj.nmd->cur_rx_ring;
	uint16_t cur_frame = pkt->fpga_hdr.frame_seq;
	if (gobj.pkts.rcvd > 0)
	{
		gobj.pkts.missed += (u_int32_t) (
			(uint16_t)(cur_frame - gobj.pkts.last_id) - 1);
	}
	gobj.pkts.rcvd++;
	gobj.pkts.last_id = cur_frame;
	gobj.pkts.inslot[ri]++;
	assert (gobj.nmd->cur_rx_ring <= gobj.nmd->last_rx_ring);
#ifdef VERBOSE
	INFO ("Packet in ring %hu, pending in ring %u\n",
			gobj.nmd->cur_rx_ring,
			nm_ring_space (NETMAP_RXRING (
					gobj.nmd->nifp,
					gobj.nmd->cur_rx_ring)));
#endif
#define LIMIT_RATE
#ifdef LIMIT_RATE
	/* limit rate */
	if (gobj.pkts.rcvd % 100 == 0)
		poll (NULL, 0, 1);
#endif

	if (gobj.pkts.rcvd + 1 == 0)
	{
		INFO ("Reached max received packets");
		raise (SIGTERM);
	}
}

static int nm_dispatch_fixed (struct nm_desc *d,
		int cnt, nm_cb_t cb, u_char *arg)
{
	int n = d->last_rx_ring - d->first_rx_ring + 1;
	int c, got = 0, ri = d->cur_rx_ring;

	if (cnt == 0)
		cnt = -1;
	/* cnt == -1 means infinite, but rings have a finite amount
	 * of buffers and the int is large enough that we never wrap,
	 * so we can omit checking for -1
	 */
	for (c=0; c < n && cnt != got; c++, ri++) {
		/* compute current ring to use */
		struct netmap_ring *ring;

		if (ri > d->last_rx_ring)
			ri = d->first_rx_ring;
		d->cur_rx_ring = ri;
		ring = NETMAP_RXRING(d->nifp, ri);
		for ( ; !nm_ring_empty(ring) && cnt != got; got++) {
			u_int i = ring->cur;
			u_int idx = ring->slot[i].buf_idx;
			u_char *buf = (u_char *)NETMAP_BUF(ring, idx);

			// __builtin_prefetch(buf);
			d->hdr.len = d->hdr.caplen = ring->slot[i].len;
			d->hdr.ts = ring->ts;
			cb(arg, &d->hdr, buf);
			ring->head = ring->cur = nm_ring_next(ring, i);
		}
	}
	return got;
}

int
main (void)
{
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
	gobj.nmd = nm_open(NMIF NMRING, NULL,
			NETMAP_NO_TX_POLL, NULL);
	if (gobj.nmd == NULL)
	{
		perror ("Could not open interface");
		exit (EXIT_FAILURE);
	}
	assert (gobj.nmd->last_rx_ring - gobj.nmd->first_rx_ring + 1 <= MAX_RINGS);
	print_desc_info ();

	/* Start the clock */
	rc = gettimeofday (&gobj.timers.start, NULL);
	if (rc == -1)
		raise (SIGTERM);

	/* Set the alarm */
	alarm (UPDATE_INTERVAL);

	/* Poll */
	struct pollfd pfd;
	pfd.fd = gobj.nmd->fd;
	pfd.events = POLLIN;
	INFO ("Starting poll\n");

	for (gobj.loop = 1, errno = 0 ;; gobj.loop++)
	{
#define DO_POLL
#ifdef DO_POLL
		rc = poll (&pfd, 1, -1);
		if (rc == -1 && errno == EINTR)
			errno = 0; /* alarm went off */
		else if (rc == -1)
			raise (SIGTERM);
		else if (rc == 0)
		{
			DEBUG ("poll timed out\n"); 
			continue;
		}
#else
		ioctl (gobj.nmd->fd, NIOCRXSYNC);
#endif

#ifdef VERBOSE
		INFO ("Dispatching\n");
#endif
		nm_dispatch_fixed (gobj.nmd, -1, rx_handler, NULL);
	}

	errno = 0;
	raise (SIGTERM); /* cleanup */
	return 0; /*never reached, suppress gcc warning*/
}
