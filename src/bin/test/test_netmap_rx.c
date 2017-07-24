#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <errno.h>
#include <assert.h>
#include <poll.h>

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>

#define FPGA_DEBUG
// #define FPGA_USE_MACROS
#include <net/fpga_user.h>

#define MAX_FSIZE  5ULL << 32 /* 20GB */
#define SAVE_FILE  "/media/nm_test"
#define UPDATE_INTERVAL 1
#define SAVE_TICKS 1000000

#define NM_IFNAME "vale:fpga"

#define ERROR(...) fprintf (stdout, __VA_ARGS__)
#define DEBUG(...) fprintf (stderr, __VA_ARGS__)
#define INFO(...)  fprintf (stdout, __VA_ARGS__)

#define USE_MMAP
// #define USE_DISPATCH

static struct
{
	struct nm_desc* nmd;
	int save_fd;
	u_int64_t b_written;
	void* save_map;
	struct {
		struct timeval start;
		struct timeval last_check;
	} timers;
	struct {
		fpga_pkt* cur_mca;
		u_int last_rcvd;
		u_int rcvd;
	} pkts;
	u_int loop;
} gobj;

static void
print_desc_info (void)
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
		u_int new_rcvd = gobj.pkts.rcvd - gobj.pkts.last_rcvd;
		INFO (
			"total received: %10u   newly received: %10u    "
			"avg bandwidth: %10.3e pps\n",
			gobj.pkts.rcvd,
			new_rcvd,
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
			"packets received:  %10u\n"
			"avg pkts per loop: %10u\n"
			"avg bandwidth:     %10.3e pps\n"
			"-----------------------------\n",
			gobj.loop,
			gobj.pkts.rcvd,
			(gobj.loop > 0) ? gobj.pkts.rcvd / gobj.loop : 0,
			(double) gobj.pkts.rcvd / tdelta
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
	if (fpgaerrno)
	{
		__fpga_perror (stderr, "");
		rc = EXIT_FAILURE;
	}

	if (gobj.nmd != NULL)
	{
		print_stats (0);
		nm_close (gobj.nmd);
	}

	if (gobj.pkts.cur_mca != NULL)
	{
		free (gobj.pkts.cur_mca);
		gobj.pkts.cur_mca = NULL;
	}

	if ( gobj.save_map != NULL && gobj.save_map != (void*)-1)
		munmap (gobj.save_map, MAX_FSIZE);

	if (gobj.b_written)
	{
		errno = 0;
		ftruncate (gobj.save_fd, gobj.b_written);
		if (errno)
			perror (""); /* non-fatal, but print info */
	}
	close (gobj.save_fd);

	exit (rc);
}

static void
rx_handler (u_char* arg, const struct nm_pkthdr* hdr, const u_char* buf)
{
	
	/* ----------------------------------------------------------------- */

	gobj.pkts.rcvd++;
	if (gobj.pkts.rcvd + 1 == 0)
	{
		errno = EOVERFLOW;
		raise (SIGTERM);
	}
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
	gobj.nmd = nm_open(NM_IFNAME"}1", NULL, 0, 0);
	if (gobj.nmd == NULL)
	{
		ERROR ("Could not open interface %s\n", NM_IFNAME);
		exit (EXIT_FAILURE);
	}
	print_desc_info ();

	/* Get the ring (we only use one) */
	assert (gobj.nmd->first_rx_ring == gobj.nmd->last_rx_ring);
	struct netmap_ring* rxring = NETMAP_RXRING (
			gobj.nmd->nifp, gobj.nmd->cur_rx_ring);

	/* Open the file */
	gobj.save_fd = open (SAVE_FILE, O_CREAT | O_RDWR,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (gobj.save_fd == -1)
		raise (SIGTERM);
	rc = posix_fallocate (gobj.save_fd, 0, MAX_FSIZE);
	if (rc)
	{
		errno = rc;
		raise (SIGTERM);
	}

#ifdef USE_MMAP
	gobj.save_map = mmap (NULL, MAX_FSIZE, PROT_WRITE,
		MAP_SHARED, gobj.save_fd, 0);
	if (gobj.save_map == (void*)-1)
		raise (SIGTERM);
#endif

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

	int cur_tick = 0;
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

#ifdef USE_DISPATCH
		nm_dispatch (gobj.nmd, -1, rx_handler, NULL);
#else
		do
		{
			u_int32_t cur_bufid =
				rxring->slot[ rxring->cur ].buf_idx;
			fpga_pkt* pkt =
				(fpga_pkt*) NETMAP_BUF (rxring, cur_bufid);

			/* ------------------------------------------------- */
			/* ------------------ save packet ------------------ */
#ifdef USE_MMAP
			memcpy ((char*)gobj.save_map + gobj.b_written, pkt,
				pkt->length);
#else
			rc = write (gobj.save_fd, pkt, pkt->length);
#endif
			gobj.b_written += pkt->length;

			if (rc == -1)
				raise (SIGTERM);
			/* ------------------------------------------------- */

			rxring->head = rxring->cur =
				nm_ring_next(rxring, rxring->cur);

			gobj.pkts.rcvd++;
			if (gobj.pkts.rcvd + 1 == 0)
			{
				errno = EOVERFLOW;
				raise (SIGTERM);
			}

			if (is_tick (pkt))
			{
				DEBUG ("Received tick #%d\n", cur_tick);
				cur_tick++;
			}
			if (gobj.b_written + MAX_FPGA_FRAME_LEN > MAX_FSIZE
				|| cur_tick == SAVE_TICKS)
				raise (SIGTERM); /* done */
		} while ( ! nm_ring_empty (rxring) );
#endif
	}

	errno = 0;
	raise (SIGTERM); /* cleanup */
	return 0; /*never reached, suppress gcc warning*/
}
