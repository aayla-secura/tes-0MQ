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

#define TESPKT_DEBUG
#include "net/tespkt.h"

#define DST_HW_ADDR "ff:ff:ff:ff:ff:ff"
#define SRC_HW_ADDR "5a:ce:be:b7:b2:91"
#define ETH_PROTO ETHERTYPE_F_EVENT

#ifndef NMRING
#define NMRING
#endif
#define MAX_RINGS 24
#define PKT_LEN MAX_TES_FRAME_LEN
#define UPDATE_INTERVAL 1
#ifndef NMIF
#define NMIF "vale0:vi0"
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
		u_int32_t last_sent;
		u_int32_t sent;
		u_int32_t inslot[MAX_RINGS];
	} pkts;
	u_int32_t loop;
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

	if (gobj.nmd)
	{
		print_stats (0);
		nm_close (gobj.nmd);
	}

	exit (rc);
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

	if (rc != 0)
	{
		perror ("sigaction");
		exit (EXIT_FAILURE);
	}

	gobj.nmd = nm_open(NMIF NMRING, NULL, 0, NULL);
	if (gobj.nmd == NULL)
	{
		perror ("Could not open interface");
		exit (EXIT_FAILURE);
	}
	assert (gobj.nmd->last_rx_ring - gobj.nmd->first_rx_ring + 1 <= MAX_RINGS);
	print_desc_info ();

	/* A dummy packet */
	struct tespkt pkt = {0};
	unsigned char body[MAX_TES_FRAME_LEN - TES_HDR_LEN] = {0};
	pkt.body = body;
	assert (sizeof (pkt) == PKT_LEN);
	struct ether_addr* mac_addr = ether_aton (DST_HW_ADDR);
	memcpy (&pkt.eth_hdr.ether_dhost, mac_addr, ETHER_ADDR_LEN);
	mac_addr = ether_aton (SRC_HW_ADDR);
	memcpy (&pkt.eth_hdr.ether_shost, mac_addr, ETHER_ADDR_LEN);
	pkt.eth_hdr.ether_type = htons (ETH_PROTO);

	/* Start the clock */
	rc = gettimeofday (&gobj.timers.start, NULL);
	if (rc == -1)
		raise (SIGTERM);

	/* Set the alarm */
	alarm (UPDATE_INTERVAL);

	/* Poll */
/* #define DO_POLL */
#ifdef DO_POLL
	struct pollfd pfd;
	pfd.fd = gobj.nmd->fd;
	pfd.events = POLLOUT;
#endif
	INFO ("\nStarting poll\n");

	for (gobj.loop = 1, errno = 0 ;; gobj.loop++)
	{
#ifdef DO_POLL
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
#else
		ioctl (gobj.nmd->fd, NIOCTXSYNC);
#endif

		if (nm_inject (gobj.nmd, &pkt, PKT_LEN))
		{
			gobj.pkts.sent++;
			gobj.pkts.inslot[gobj.nmd->cur_tx_ring]++;
			pkt.tes_hdr.fseq++;
#define ADV_RIDX
#define RAND_RIDX
#ifdef ADV_RIDX
#ifndef RAND_RIDX
			/* advance the ring for next time */
			if (gobj.nmd->cur_tx_ring == gobj.nmd->first_tx_ring)
				gobj.nmd->cur_tx_ring = gobj.nmd->last_tx_ring;
			else
				gobj.nmd->cur_tx_ring--;
#else /* RAND_RIDX */
			/* set it to random */
			u_int16_t nr = gobj.nmd->last_rx_ring - gobj.nmd->first_rx_ring + 1;
			int ridx = (int) ((double)rand () * nr / RAND_MAX);
			/* only if rand returned RAND_MAX */
			if (ridx == nr)
				ridx--;
			assert (ridx >= 0 && ridx < nr);
			gobj.nmd->cur_tx_ring = ridx;
#endif /* RAND_RIDX */
#endif /* ADV_RIDX */
		}
#define LIMIT_RATE
#ifdef LIMIT_RATE
		/* limit rate */
		if (gobj.pkts.sent % 50 == 0)
			poll (NULL, 0, 1);
#endif
	}

	errno = 0;
	raise (SIGTERM); /*cleanup*/
	return 0; /*never reached, suppress gcc warning*/
}
