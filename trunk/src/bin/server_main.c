/*
 * See api.h
 *
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * –––––––––––––––––––––––––––––––– DEV NOTES –––––––––––––––––––––––––––––––––
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * When debugging we use assert throughout to catch bugs. Normally these
 * statements should never be reached regardless of user input. Other errors
 * are handled gracefully with messages to clients and/or syslog or stderr/out.
 *
 * There is a separate thread for each "task" (see fpgatasks.h). Tasks are
 * started with tasks_start. Each task has read-only access to rings (they
 * cannot modify the cursor or head) and each task keeps its own head, which is
 * visible by the coordinator (fpgacoord.h).
 *
 * After receiving new packets, the coordinator sets the true cursor and head
 * to the per-task head which lags behind all others (tasks_head).
 * Then, to each task which is waiting for more packets it sends a SIG_WAKEUP
 * (tasks_wakeup). 
 *
 * Tasks receiving SIG_WAKEUP must process packets, advancing their head until
 * there are no more packets or until they are no longer interested (in which
 * case they set an 'active' boolean to false and will no longer receive
 * SIG_WAKEUP).
 *
 * The coordinator must register a generic task reader with its zloop, so that
 * when tasks encounter an error the coordinator's loop is terminated. The
 * signal handler is generic, internal to fpgatasks. Coordinator simply passes
 * the loop to tasks_start and after exiting from its loop (for whatever reason)
 * calls tasks_stop to shutdown all tasks cleanly.
 *
 * Note: bool type and true/false macros are ensured by CZMQ.
 *
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * –––––––––––––––––––––––––––––––––– TO DO –––––––––––––––––––––––––––––––––––
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * Try
 *   aio_write
 *   buffering to a separate (mmapped) area and writing bigger chunks to disk
 *   zsys_io_threads
 */

#include "fpgatasks.h"
#include "net/fpgaif_manager.h"
#include "common.h"
#include <net/if.h> /* IFNAMSIZ */

#define UPDATE_INTERVAL 1             // in seconds
#define FPGA_IF         "vale:fpga}1" // default

/*
 * Statistics, only used in foreground mode
 */
struct stats_t
{
	struct    timeval last_update;
	u_int64_t received;
	u_int64_t missed;
};

struct data_t
{
	struct stats_t stats;
	ifring* rxring;
};

static void usage (const char* self);
static int  print_stats (zloop_t* loop, int timer_id, void* stats_);
static int  new_pkts_hn (zloop_t* loop, zmq_pollitem_t* pitem, void* data_);
static int  coordinator_body (const char* ifname);

/* ------------------------------------------------------------------------- */

static void
usage (const char* self)
{
	fprintf (stderr,
		"Usage: %s [options]\n\n"
		"Options:\n"
		"    -i <if>              Read packets from <if> interface\n"
		"                         Defaults to "FPGA_IF"\n"
		"    -f                   Run in foreground\n"
		"    -v                   Print debugging messages\n", self
		);
	exit (EXIT_FAILURE);
}

/*
 * When working in foreground, print statistics (bandwidth, etc) every
 * UPDATE_INTERVAL.
 */
static int
print_stats (zloop_t* loop, int timer_id, void* stats_)
{
	assert (stats_);
	struct stats_t* stats = (struct stats_t*) stats_;

	if ( ! timerisset (&stats->last_update) )
	{ /* first time */
		gettimeofday (&stats->last_update, NULL);
		return 0;
	}

	struct timeval tnow, tdiff;
	gettimeofday (&tnow, NULL);

	timersub (&tnow, &stats->last_update, &tdiff);
	double tdelta = tdiff.tv_sec + 1e-6 * tdiff.tv_usec;
	
	s_msgf (0, LOG_INFO, 0, 
		"elapsed: %2.5f received: %7lu dropped: %7lu "
		"avg bandwidth: %10.3e pps",
		tdelta,
		stats->received,
		stats->missed,
		(double) stats->received / tdelta
		);

	memcpy (&stats->last_update, &tnow, sizeof (struct timeval));
	stats->received = 0;
	stats->missed = 0;

	return 0;
}

/*
 * Called when new packets arrive in the ring.
 */
static int
new_pkts_hn (zloop_t* loop, zmq_pollitem_t* pitem, void* data_)
{
	assert (data_);
	struct data_t* data = (struct data_t*) data_;

	/* Signal the waiting tasks and find the head of the slowest one */
	u_int32_t head = ifring_tail (data->rxring); /* if no active tasks */
	tasks_get_head (&head);
	int rc = tasks_wakeup ();
	if (rc)
	{
		s_msg (0, LOG_DEBUG, 0,
			"Could not wake up all waiting tasks.");
		return -1;
	}

	if ( ! is_daemon )
	{
		/* Save statistics */
		uint32_t num_new = ifring_pending (data->rxring);
		data->stats.received += num_new;
		fpga_pkt* pkt = (fpga_pkt*) ifring_cur_buf (data->rxring);
		uint16_t fseqA = frame_seq (pkt);
		uint16_t fseqB = frame_seq (pkt);
		data->stats.missed += (u_int64_t) ( num_new - 1 -
				(uint16_t)(fseqB - fseqA) );
	}

	/* Set the head and cursor */
	ifring_goto_buf (data->rxring, head);
	ifring_release_to_buf (data->rxring, head);

	return 0;
}

static int
coordinator_body (const char* ifname)
{
	int rc;
	struct data_t data;
	memset (&data, 0, sizeof (data));

	/* Open the interface */
	ifdesc* ifd = if_open (ifname, NULL, 0, 0);
	if (ifd == NULL)
	{
		s_msgf (errno, LOG_ERR, 0, "Could not open interface %s",
			ifname);
		return -1;
	}
	s_msgf (0, LOG_INFO, 0, "Opened interface %s", ifname);

	/* Get the ring, we support only one for now */
	assert (if_rxrings (ifd) == 1);
	data.rxring = if_first_rxring (ifd);

	/* Start the tasks and register the readers. Tasks are initialized as
	 * inactive. */
	zloop_t* loop = zloop_new ();
	rc = tasks_start (data.rxring, loop);
	if (rc)
	{
		s_msg (0, LOG_DEBUG, 0, "Tasks failed to start");
		goto cleanup;
	}

	/* Register the FPGA interface as a poller */
	struct zmq_pollitem_t pitem;
	memset (&pitem, 0, sizeof (pitem));
	pitem.fd = if_fd (ifd);
	pitem.events = ZMQ_POLLIN;
	rc = zloop_poller (loop, &pitem, new_pkts_hn, &data);
	if (rc == -1)
	{
		s_msg (errno, LOG_ERR, 0,
			"Could not register the zloop poller");
		goto cleanup;
	}

	if ( ! is_daemon )
	{
		/* Set the timer */
		rc = zloop_timer (loop, 1000 * UPDATE_INTERVAL, 0,
			print_stats, &data.stats);
		if (rc == -1)
		{
			s_msg (errno, LOG_ERR, 0, "Could not set a timer");
			goto cleanup;
		}
		s_msgf (0, LOG_DEBUG, 0, "Will print stats every %d seconds",
			UPDATE_INTERVAL);
	}

	s_msg (0, LOG_DEBUG, 0, "All threads initialized");
	rc = zloop_start (loop);

	if (rc)
	{
		s_msg (0, LOG_DEBUG, 0, "Terminated by handler");
	}
	else
	{
		s_msg (0, LOG_DEBUG, 0, "Interrupted");
	}

cleanup:
	tasks_destroy ();
	zloop_destroy (&loop);
	if_close (ifd);
	s_msg (0, LOG_DEBUG, 0, "Done");
	return rc;
}

int
main (int argc, char **argv)
{
	// __fpga_self_test ();
	int rc;

	/* Process command-line options */
	is_daemon = 1;
	is_verbose = 0;
	int opt;
	char ifname[IFNAMSIZ + 1];
	memset (ifname, 0, sizeof (ifname));
	while ( (opt = getopt (argc, argv, "i:fv")) != -1 )
	{
		switch (opt)
		{
			case 'i':
				snprintf (ifname, sizeof (ifname),
					"%s", optarg);
				break;
			case 'f':
				is_daemon = 0;
				break;
			case 'v':
				is_verbose = 1;
				break;
			case '?':
				usage (argv[0]);
				break;
			default:
				/* we forgot to handle an option */
				assert (0);
		}
	}
	if (strlen (ifname) == 0)
	{
		sprintf (ifname, FPGA_IF);
	}

	if (is_daemon)
	{
		/* Go into background */
		/* TO DO: set a pidfile */
		rc = daemonize (NULL);
		if (rc == -1)
		{
			s_msg (errno, LOG_ERR, 0,
				"Failed to go into background");
			exit (EXIT_FAILURE);
		}

		/* Start syslog */
		openlog ("FPGA server", 0, LOG_DAEMON);
	}

	rc = coordinator_body (ifname);
	exit ( rc ? EXIT_FAILURE : EXIT_SUCCESS );
}
