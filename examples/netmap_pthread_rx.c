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
#include <pthread.h>

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>

#define TESPKT_DEBUG
#include "net/tespkt.h"

#define MAX_FSIZE  5ULL << 32 /* 20GB */
// #define SAVE_FILE  "/media/nm_test"
#define UPDATE_INTERVAL 1
#define MAX_TICKS 1000000 /* Set to 0 for unlimited */

#define NM_IFNAME "vale0:vi1"

#define ERROR(...) fprintf (stdout, __VA_ARGS__)
#define DEBUG(...) fprintf (stderr, __VA_ARGS__)
#define INFO(...)  fprintf (stdout, __VA_ARGS__)

#define USE_MMAP
// #define USE_DISPATCH

/* Global stats */
static struct
{
#ifdef SAVE_FILE
	u_int64_t b_written;
#endif /* SAVE_FILE */
	struct {
		struct timeval start;
		struct timeval last_check;
	} timers;
	struct {
		tespkt* cur_mca;
		u_int32_t last_rcvd;
		u_int32_t rcvd;
		u_int32_t ticks;
		u_int32_t missed;
	} pkts;
	u_int32_t loop;
	struct {
		pthread_t th_id;
		int pipefd[2];
	} tes_th;
} gstats;

/* Data managed by the thread communicating with the TES */
struct t_data
{
	struct nm_desc* nmd;
#ifdef SAVE_FILE
	int save_fd;
	void* save_map;
#endif /* SAVE_FILE */
};

static void
print_desc_info (struct nm_desc* nmd)
{
	INFO (
		"\n-----------------------------\n"
		"ringid: %hu, flags: %u, cmd: %hu\n"
		"extra rings: %hu, extra buffers: %u\n"
		"done_mmap: %d\n"
		"rx rings: %hu, rx slots: %u\n"
		"tx rings: %hu, tx slots: %u\n"
		"first rx: %hu, last rx: %hu\n"
		"first tx: %hu, last tx: %hu\n"
		"snaplen: %d\npromisc: %d\n"
		"-----------------------------\n\n",
		nmd->req.nr_ringid,
		nmd->req.nr_flags,
		nmd->req.nr_cmd,
		nmd->req.nr_arg1,
		nmd->req.nr_arg3,
		nmd->done_mmap,
		nmd->req.nr_rx_rings,
		nmd->req.nr_rx_slots,
		nmd->req.nr_tx_rings,
		nmd->req.nr_tx_slots,
		nmd->first_rx_ring,
		nmd->last_rx_ring,
		nmd->first_tx_ring,
		nmd->last_tx_ring,
		nmd->snaplen,
		nmd->promisc
		);
}

static void
print_stats (int sig)
{
	if ( ! timerisset (&gstats.timers.start) )
		return;

	struct timeval* tprev = &gstats.timers.last_check;
	if ( ( ! timerisset (tprev) ) || sig == 0 )
		tprev = &gstats.timers.start;

	struct timeval tnow, tdiff;
	gettimeofday (&tnow, NULL);

	timersub (&tnow, tprev, &tdiff);
	double tdelta = tdiff.tv_sec + 1e-6 * tdiff.tv_usec;
	
	if (sig)
	{
		assert (sig == SIGALRM);
		/* Alarm went off, update stats */
		u_int32_t new_rcvd = gstats.pkts.rcvd - gstats.pkts.last_rcvd;
		INFO (
			"ticks: %10u ; total pkts received: %10u ; "
			/* "new pkts received: %10u ; " */
			"avg bandwidth: %10.3e pps\n",
			gstats.pkts.ticks,
			gstats.pkts.rcvd,
			/* new_rcvd, */
			(double) new_rcvd / tdelta
			);

		memcpy (&gstats.timers.last_check, &tnow,
			sizeof (struct timeval));
		gstats.pkts.last_rcvd = gstats.pkts.rcvd;

		/* Set alarm again */
		alarm (UPDATE_INTERVAL);
	}
	else
	{
		/* Called by main_cleanup, print final stats */
		INFO (
			"\n-----------------------------\n"
			"looped:            %10u\n"
			"ticks:             %10u\n"
			"packets received:  %10u\n"
			"packets missed:    %10u\n"
			"avg pkts per loop: %10u\n"
			"avg bandwidth:     %10.3e pps\n"
			"-----------------------------\n",
			gstats.loop,
			gstats.pkts.ticks,
			gstats.pkts.rcvd,
			gstats.pkts.missed,
			(gstats.loop > 0) ? gstats.pkts.rcvd / gstats.loop : 0,
			(double) gstats.pkts.rcvd / tdelta
			);
	}
}

static void
cleanup (void* data_)
{
	// sleep (1); /* for debugging */
	struct t_data* data = (struct t_data*) data_;

	if (data->nmd != NULL)
		nm_close (data->nmd);

#ifdef SAVE_FILE
#ifdef USE_MMAP
	if ( data->save_map != NULL && data->save_map != (void*)-1)
		munmap (data->save_map, MAX_FSIZE);
#endif /* USE_MMAP */

	if (data->save_fd >= 0)
	{
		int rc = ftruncate (data->save_fd, gstats.b_written);
		if (rc == -1)
			perror (""); /* non-fatal, but print info */
		close (data->save_fd);
	}
#endif /* SAVE_FILE */
	DEBUG ("Cleaned up\n");
	write (gstats.tes_th.pipefd[1], "0", 1); /* signal to main thread */
}

static void
main_cleanup (int sig)
{
	if (sig == SIGINT)
		INFO ("Interrupted\n");

	if (sig)
		print_stats (0);

	int rc = EXIT_SUCCESS;
	if (errno && errno != EINTR)
	{
		perror ("");
		rc = EXIT_FAILURE;
	}

	if (gstats.pkts.cur_mca != NULL)
	{
		free (gstats.pkts.cur_mca);
		gstats.pkts.cur_mca = NULL;
	}

	if ( pthread_cancel (gstats.tes_th.th_id) == 0)
	{
		if ( (errno = pthread_join (gstats.tes_th.th_id, NULL)) )
			perror ("");
	}

	exit (rc);
}

static void
rx_handler (u_char* data_, const struct nm_pkthdr* hdr, const u_char* buf)
{
	/* To do */
}

static void*
main_body (void* arg)
{
	/* arg is ignored */
	int rc;

	rc = pthread_setcancelstate (PTHREAD_CANCEL_ENABLE, NULL);
	if (rc)
	{
		errno = rc; /* pthread_* do not set it */
		ERROR ("Could not enable thread cancellation\n");
		pthread_exit (NULL);
	}

	rc = pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	if (rc)
	{
		errno = rc; /* pthread_* do not set it */
		ERROR ("Could not set thread cancel state to async\n");
		pthread_exit (NULL);
	}

	struct t_data data;
	memset (&data, 0, sizeof (data));
#ifdef SAVE_FILE
	data.save_fd = -1;
#endif /* SAVE_FILE */

	/* Open the interface */
	data.nmd = nm_open(NM_IFNAME"}1", NULL, 0, 0);
	if (data.nmd == NULL)
	{
		// TO DO: Move the error reporting to the main thread?
		ERROR ("Could not open interface %s\n", NM_IFNAME);
		pthread_exit (NULL);
	}
	INFO ("Opened interface %s\n", NM_IFNAME"}1");
	print_desc_info (data.nmd);

	/* Set up thread destructors */
	pthread_cleanup_push (cleanup, &data);

	/* Get the ring (we only use one) */
	assert (data.nmd->first_rx_ring == data.nmd->last_rx_ring);
	struct netmap_ring* rxring = NETMAP_RXRING (
		data.nmd->nifp, data.nmd->first_rx_ring);

#ifdef SAVE_FILE
	/* Open the file */
	data.save_fd = open (SAVE_FILE, O_CREAT | O_RDWR,
		S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (data.save_fd == -1)
	{
		ERROR ("Could not open %s\n", SAVE_FILE);
		pthread_exit (NULL);
	}
	INFO ("Opened file %s for writing\n", SAVE_FILE);

	rc = posix_fallocate (data.save_fd, 0, MAX_FSIZE);
	if (rc)
	{
		ERROR ("Could not allocate sufficient space\n");
		errno = rc; /* posix_fallocate does not set it */
		pthread_exit (NULL);
	}
	DEBUG ("Allocated %lld bytes to the file\n", MAX_FSIZE);

#ifdef USE_MMAP
	data.save_map = mmap (NULL, MAX_FSIZE, PROT_WRITE,
		MAP_SHARED, data.save_fd, 0);
	if (data.save_map == (void*)-1)
	{
		ERROR ("Could not mmap file %s\n", SAVE_FILE);
		pthread_exit (NULL);
	}
	DEBUG ("Done mmap\n");
#endif /* USE_MMAP */
#endif /* SAVE_FILE */

	/* Start the clock */
	rc = gettimeofday (&gstats.timers.start, NULL);
	if (rc == -1)
	{
		ERROR ("Could not get system time\n");
		pthread_exit (NULL);
	}

	/* Poll */
	struct pollfd pfd;
	pfd.fd = data.nmd->fd;
	pfd.events = POLLIN;
	DEBUG ("Starting poll\n");

	uint16_t cur_frame = -1;
	for (gstats.loop = 1, errno = 0 ;; gstats.loop++)
	{
		rc = poll (&pfd, 1, -1);
		if (rc == -1)
		{
			ERROR ("Error while polling\n");
			pthread_exit (NULL);
		}
		assert (rc == 1);

#ifdef USE_DISPATCH
		nm_dispatch (data.nmd, -1, rx_handler, data);
#else
		do
		{
			u_int32_t cur_bufid =
				rxring->slot[ rxring->cur ].buf_idx;
			tespkt* pkt =
				(tespkt*) NETMAP_BUF (rxring, cur_bufid);

#ifdef SAVE_FILE
			/* ------------------------------------------------- */
			/* ------------------ save packet ------------------ */
#ifdef USE_MMAP
			memcpy ((char*)data.save_map + gstats.b_written, pkt,
				pkt->length);
#else /* USE_MMAP */
			ssize_t wrc = write (data.save_fd, pkt, pkt->length);
			if (wrc == -1)
			{
				ERROR ("Could not write to file\n");
				pthread_exit (NULL);
			}
#endif /* USE_MMAP */
			gstats.b_written += pkt->length;
			/* ------------------------------------------------- */
#endif /* SAVE_FILE */

			rxring->head = rxring->cur =
				nm_ring_next(rxring, rxring->cur);

			if (gstats.pkts.rcvd > 0)
			{
				uint16_t prev_frame = cur_frame;
				cur_frame = pkt->tes_hdr.fseq;
				gstats.pkts.missed += (u_int32_t) (
					(uint16_t)(cur_frame - prev_frame) - 1);
			}
			else
			{
				cur_frame = pkt->tes_hdr.fseq;
				INFO ("First received frame is #%hu\n", cur_frame);
			}

			gstats.pkts.rcvd++;
			if (gstats.pkts.rcvd + 1 == 0)
			{
				errno = EOVERFLOW;
				pthread_exit (NULL);
			}

			if (tespkt_is_tick (pkt))
			{
				gstats.pkts.ticks++;
				// DEBUG ("Received tick #%d\n", gstats.pkts.ticks);
			}
			if (MAX_TICKS > 0 && gstats.pkts.ticks == MAX_TICKS)
				pthread_exit (NULL); /* done */

#ifdef SAVE_FILE
			if (gstats.b_written + MAX_TES_FRAME_LEN > MAX_FSIZE)
				pthread_exit (NULL); /* done */
#endif /* SAVE_FILE */
		} while ( ! nm_ring_empty (rxring) );
#endif /* USE_DISPATCH */
	}

	/* Cleanup */
	pthread_cleanup_pop (1);
	return NULL;
}

int
main (void)
{
	int rc;

	/* Open the pipe for talking to the TES thread */
	rc = pipe (gstats.tes_th.pipefd);
	if (rc == -1)
	{
		ERROR ("Could not open a pipe\n");
		exit (EXIT_FAILURE);
	}

	/* Block signals before starting the thread, unblock it later */
	sigset_t sigmask;
	sigemptyset (&sigmask);
	sigaddset (&sigmask, SIGINT);
	sigaddset (&sigmask, SIGTERM);
	sigaddset (&sigmask, SIGALRM);
	rc = pthread_sigmask (SIG_BLOCK, &sigmask, NULL);
	if (rc == -1)
	{
		ERROR ("Could not block signals prior to thread "
			"initialisation\n");
		exit (EXIT_FAILURE);
	}

	/* Start the thread */
	pthread_attr_t t_attr;
	rc = pthread_attr_init (&t_attr);
	if (rc)
	{
		errno = rc; /* pthread_* do not set it */
		ERROR ("Could not initialize thread attributes\n");
		exit (EXIT_FAILURE);
	}
	rc = pthread_attr_setdetachstate (&t_attr, PTHREAD_CREATE_JOINABLE);
	if (rc)
	{
		errno = rc; /* pthread_* do not set it */
		ERROR ("Could not set thread attributes\n");
		exit (EXIT_FAILURE);
	}

	rc = pthread_create (&gstats.tes_th.th_id, &t_attr, main_body, NULL);
	if (rc)
	{
		errno = rc; /* pthread_* do not set it */
		ERROR ("Could not start TES thread\n");
		exit (EXIT_FAILURE);
	}
	DEBUG ("Started TES thread\n");

	/* Setup signal handlers and unblock signals */
	struct sigaction sigact;

	sigact.sa_flags = 0;
	sigact.sa_handler = main_cleanup;
	sigemptyset (&sigact.sa_mask);
	sigaddset (&sigact.sa_mask, SIGINT);
	sigaddset (&sigact.sa_mask, SIGTERM);
	sigaddset (&sigact.sa_mask, SIGALRM);
	rc = sigaction (SIGINT, &sigact, NULL);
	rc |= sigaction (SIGTERM, &sigact, NULL);

	sigact.sa_handler = print_stats;
	sigemptyset (&sigact.sa_mask);
	sigaddset (&sigact.sa_mask, SIGALRM);
	rc |= sigaction (SIGALRM, &sigact, NULL);

	if (rc == -1)
	{
		ERROR ("Could not install signal handlers\n");
		main_cleanup (0);
	}

	rc = pthread_sigmask (SIG_UNBLOCK, &sigmask, NULL);
	if (rc == -1)
	{
		ERROR ("Could not unblock signals\n");
		main_cleanup (0);
	}

	/* Set the alarm */
	alarm (UPDATE_INTERVAL);

	/* Do stuff */
	struct pollfd pfd;
	pfd.fd = gstats.tes_th.pipefd[0];
	pfd.events = POLLIN;
	do
	{
		rc = poll (&pfd, 1, -1);
	} while (rc == -1 && errno == EINTR);

	DEBUG ("Done, exiting\n");
	main_cleanup (SIGTERM);
}
