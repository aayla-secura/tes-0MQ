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

#define FPGAPKT_DEBUG
// #define FPGA_USE_MACROS
#include <net/fpgapkt.h>

#define MAX_FSIZE  5ULL << 32 /* 20GB */
// #define SAVE_FILE  "/media/nm_test"
#define UPDATE_INTERVAL 1
#define MAX_TICKS 10000000 /* Set to 0 for unlimited */

#define NM_IFNAME "vale:fpga"

#define ERROR(...) fprintf (stdout, __VA_ARGS__)
#define DEBUG(...) fprintf (stderr, __VA_ARGS__)
#define INFO(...)  fprintf (stdout, __VA_ARGS__)

#define USE_MMAP
// #define USE_DISPATCH

static struct
{
	struct nm_desc* nmd;
#ifdef SAVE_FILE
	int save_fd;
	void* save_map;
	u_int64_t b_written;
#endif /* SAVE_FILE */
	struct {
		struct timeval start;
		struct timeval last_check;
	} timers;
	struct {
		fpga_pkt* cur_mca;
		u_int32_t last_rcvd;
		u_int32_t rcvd;
		u_int32_t ticks;
		u_int32_t missed;
	} pkts;
	u_int32_t loop;
} gobj;

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
		u_int32_t new_rcvd = gobj.pkts.rcvd - gobj.pkts.last_rcvd;
		INFO (
			"ticks: %10u ; total pkts received: %10u ; "
			/* "new pkts received: %10u ; " */
			"avg bandwidth: %10.3e pps\n",
			gobj.pkts.ticks,
			gobj.pkts.rcvd,
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
			"ticks:             %10u\n"
			"packets received:  %10u\n"
			"packets missed:    %10u\n"
			"avg pkts per loop: %10u\n"
			"avg bandwidth:     %10.3e pps\n"
			"-----------------------------\n",
			gobj.loop,
			gobj.pkts.ticks,
			gobj.pkts.rcvd,
			gobj.pkts.missed,
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

#ifdef SAVE_FILE
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
#endif /* SAVE_FILE */

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

#ifdef SAVE_FILE
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
#endif /* USE_MMAP */
#endif /* SAVE_FILE */

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

	uint16_t cur_frame = -1;
	for (gobj.loop = 1, errno = 0 ;; gobj.loop++)
	{
		rc = poll (&pfd, 1, 1000);
		if (rc == -1 && errno == EINTR)
		{
			errno = 0;
			continue;
		}
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

#ifdef SAVE_FILE
			/* ------------------------------------------------- */
			/* ------------------ save packet ------------------ */
#ifdef USE_MMAP
			memcpy ((char*)gobj.save_map + gobj.b_written, pkt,
				pkt->length);
#else /* USE_MMAP */
			rc = write (gobj.save_fd, pkt, pkt->length);
#endif /* USE_MMAP */
			gobj.b_written += pkt->length;

			if (rc == -1)
				raise (SIGTERM);
			/* ------------------------------------------------- */
#endif /* SAVE_FILE */

			rxring->head = rxring->cur =
				nm_ring_next(rxring, rxring->cur);

			if (gobj.pkts.rcvd > 0)
			{
				uint16_t prev_frame = cur_frame;
				cur_frame = pkt->fpga_hdr.frame_seq;
				gobj.pkts.missed += (u_int32_t) (
					(uint16_t)(cur_frame - prev_frame) - 1);
			}
			else
			{
				cur_frame = pkt->fpga_hdr.frame_seq;
				INFO ("First received frame is #%hu\n", cur_frame);
			}

			gobj.pkts.rcvd++;
			if (gobj.pkts.rcvd + 1 == 0)
			{
				errno = EOVERFLOW;
				raise (SIGTERM);
			}

			if (is_tick (pkt))
			{
				gobj.pkts.ticks++;
				DEBUG ("Received tick #%d\n", gobj.pkts.ticks);
			}
			if (MAX_TICKS > 0 && gobj.pkts.ticks == MAX_TICKS)
				raise (SIGTERM); /* done */

#ifdef SAVE_FILE
			if (gobj.b_written + MAX_FPGA_FRAME_LEN > MAX_FSIZE)
				raise (SIGTERM); /* done */
#endif /* SAVE_FILE */
		} while ( ! nm_ring_empty (rxring) );
#endif /* USE_DISPATCH */
	}

	errno = 0;
	raise (SIGTERM); /* cleanup */
	return 0; /*never reached, suppress gcc warning*/
}
