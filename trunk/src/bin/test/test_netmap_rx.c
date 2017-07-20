#define NETMAP_WITH_LIBS

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
#include <net/netmap_user.h>

#define NM_IFNAME "vale:fpga"

#define ERROR(...) fprintf (stdout, __VA_ARGS__)
#define PERROR(msg) perror (msg)
#define DEBUG(...) fprintf (stderr, __VA_ARGS__)
#define INFO(...) fprintf (stdout, __VA_ARGS__)

static struct
{
	struct timeval time_start;
	struct timeval time_end;
	struct timeval time_diff;
	struct nm_desc* nmd;
	/* fpga_pkt* pkt; */
	u_int loop;
} gobj;

static void print_desc_info ()
{
	INFO (
		"ringid: %hu, flags: %u, cmd: %hu\n"
		"extra rings: %hu, extra buffers: %u\n"
		"done_mmap: %d\n"
		"rx rings: %d, rx slots: %d\n"
		"tx rings: %d, tx slots: %d\n"
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

static void print_stats (void)
{
	if (gobj.time_start.tv_sec == 0)
		return;

	timersub (&gobj.time_end, &gobj.time_start, &gobj.time_diff);
	double tdiff = gobj.time_diff.tv_sec + 1e-6 * gobj.time_diff.tv_usec;
	
	INFO (
		"looped:\t\t\t%u\n"
		"received:\t\t%u\n"
		/* "dropped by OS:\t%u\n" */
		/* "dropped by IF:\t%u\n" */
		"avg pkts per loop:\t%u\n"
		"avg bandwidth:\t\t%.3Le pps\n",
		gobj.loop,
		gobj.nmd->st.ps_recv,
		/* gobj.nmd->st.ps_drop, */
		/* gobj.nmd->st.ps_ifdrop, */
		(gobj.loop > 0) ? gobj.nmd->st.ps_recv / gobj.loop : 0,
		(long double) gobj.nmd->st.ps_recv / tdiff
		);
}

static void cleanup (int sig)
{
	INFO ("Received %d\n", sig);

	int rc = EXIT_SUCCESS;
	if (errno)
	{
		PERROR ("");
		rc = EXIT_FAILURE;
	}

	gettimeofday (&gobj.time_end, NULL);
	print_stats ();
	nm_close (gobj.nmd);
	exit (rc);
}

int main (void)
{
	int rc;

	/* Signal handlers */
	struct sigaction sigact;
	sigact.sa_flags = 0;
	sigact.sa_handler = cleanup;
	rc = sigaction (SIGINT, &sigact, NULL);
	rc |= sigaction (SIGTERM, &sigact, NULL);
	if (rc == -1)
	{
		PERROR ("sigaction");
		exit (EXIT_FAILURE);
	}

	/* Open the interface */
	gobj.nmd = nm_open(NM_IFNAME"}1", NULL, 0, 0);
	if (gobj.nmd == NULL) {
		ERROR ("Could not open interface %s\n", NM_IFNAME);
		exit (EXIT_FAILURE);
	}

	print_desc_info ();

	/* Get the ring (we only use one) */
	assert (gobj.nmd->first_rx_ring == gobj.nmd->last_rx_ring);
	struct netmap_ring* rxring = NETMAP_RXRING (
			gobj.nmd->nifp, gobj.nmd->cur_rx_ring);

	/* Start the clock */
	rc = gettimeofday (&gobj.time_start, NULL);
	if (rc == -1)
	{
		PERROR ("gettimeofday");
		exit (EXIT_FAILURE);
	}

	/* Poll */
	struct pollfd pfd;
	pfd.fd = gobj.nmd->fd;
	pfd.events = POLLIN;
	DEBUG ("Starting poll\n");

	for (gobj.loop = 1, errno = 0 ;; gobj.loop++)
	{
		rc = poll (&pfd, 1, 100);
		if (rc == -1)
		{
			PERROR ("poll");
			break;
		}
		if (rc == 0)
		{
			INFO ("poll timed out\n");
			break;
		}

		/* nm_dispatch (gobj.nmd, -1, rx_handler, NULL); */
		do
		{
			/* rc = nm_inject (gobj.nmd, gobj.pkt, gobj.pkt->length); */
			u_int32_t cur_bufid = rxring->slot[ rxring->cur ].buf_idx;
			char* cur_buf = NETMAP_BUF (rxring, cur_bufid);

			rxring->head = rxring->cur = nm_ring_next(rxring, rxring->cur);
			gobj.nmd->st.ps_recv++;
			if (gobj.nmd->st.ps_recv + 1 == 0)
			{
				errno = EOVERFLOW;
				raise (SIGINT);
			}
		} while ( ! nm_ring_empty (rxring) );
	}

	errno = 0;
	raise (SIGTERM); /* cleanup */
}
