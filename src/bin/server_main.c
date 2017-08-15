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
 */

#include "fpgatasks.h"
#include "net/fpgaif_manager.h"
#include "common.h"

#ifdef BE_DAEMON
#  define SYSLOG /* print to syslog */
#  include "daemon.h"
#else /* BE_DAEMON */
#  define UPDATE_INTERVAL 1  /* in seconds */
#endif /* BE_DAEMON */

#define FPGA_IF      "vale:fpga}1"

#ifndef BE_DAEMON
/*
 * Statistics, only used in foreground mode
 */
struct stats_t
{
	struct    timeval last_update;
	u_int64_t received;
	u_int64_t missed;
};
#endif /* BE_DAEMON */

struct data_t
{
#ifndef BE_DAEMON
	struct stats_t stats;
#endif
	ifring* rxring;
};

#ifndef BE_DAEMON
static int print_stats (zloop_t* loop, int timer_id, void* stats_);
#endif /* BE_DAEMON */
static int new_pkts_hn (zloop_t* loop, zmq_pollitem_t* pitem, void* data_);
static int coordinator_body (void);

/* ------------------------------------------------------------------------- */

#ifndef BE_DAEMON
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
	
	INFO (
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
#endif /* BE_DAEMON */

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
		printf ("Could not send SIG_WAKEUP to all waiting tasks.");
		return -1;
	}

#ifndef BE_DAEMON
	/* Save statistics */
	uint32_t num_new = ifring_pending (data->rxring);
	data->stats.received += num_new;
	do
	{
		fpga_pkt* pkt = (fpga_pkt*) ifring_cur_buf (data->rxring);
		if (pkt == NULL)
		{
			WARN ("Got a NULL bufer: "
				"head at %hu, cur at %hu, tail at %hu",
					ifring_head (data->rxring),
					ifring_cur (data->rxring),
					ifring_tail (data->rxring)
			     );
			break;
		}
		uint16_t fseqA = frame_seq (pkt);
		pkt = (fpga_pkt*) ifring_last_buf (data->rxring);
		if (pkt == NULL)
		{
			WARN ("Got a NULL buffer: "
				"head at %hu, cur at %hu, tail at %hu",
					ifring_head (data->rxring),
					ifring_cur (data->rxring),
					ifring_tail (data->rxring)
			     );
			break;
		}
		uint16_t fseqB = frame_seq (pkt);
		data->stats.missed += (u_int64_t) ( num_new - 1 -
				(uint16_t)(fseqB - fseqA) );
	} while (0);
#endif /* BE_DAEMON */

	/* Set the head and cursor */
	ifring_goto (data->rxring, head, 1);

	return 0;
}

static int
coordinator_body (void)
{
	int rc;
	struct data_t data;
	memset (&data, 0, sizeof (data));

	/* Open the interface */
	ifdesc* ifd = if_open (FPGA_IF, NULL, 0, 0);
	if (ifd == NULL)
	{
		ERROR ("Could not open interface %s", FPGA_IF);
		return -1;
	}
	INFO ("Opened interface %s", FPGA_IF);

	/* Get the ring, we support only one for now */
	assert (if_rxrings (ifd) == 1);
	data.rxring = if_first_rxring (ifd);

	/* Start the tasks and register the readers. Tasks are initialized as
	 * inactive. */
	zloop_t* loop = zloop_new ();
	rc = tasks_start (data.rxring, loop);
	if (rc)
	{
		DEBUG ("Tasks failed to start");
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
		ERROR ("Could not register the zloop poller");
		goto cleanup;
	}

#ifndef BE_DAEMON
	/* Set the timer */
	rc = zloop_timer (loop, 1000 * UPDATE_INTERVAL, 0,
		print_stats, &data.stats);
	if (rc == -1)
	{
		ERROR ("Could not set a timer");
		goto cleanup;
	}
	DEBUG ("Will print stats every %d seconds", UPDATE_INTERVAL);
#endif /* BE_DAEMON */

	DEBUG ("All threads initialized");
	rc = zloop_start (loop);

	if (rc)
	{
		DEBUG ("Terminated by handler");
	}
	else
	{
		DEBUG ("Interrupted");
	}

cleanup:
	tasks_destroy ();
	zloop_destroy (&loop);
	if_close (ifd);
	DEBUG ("Done");
	return rc;
}

int
main (void)
{
	// __fpga_self_test ();
	int rc;

#ifdef BE_DAEMON
	/* Go into background */
	rc = daemonize ();
	if (rc == -1)
	{
		ERROR ("Failed to go into background");
		exit (EXIT_FAILURE);
	}

	/* Start syslog */
	openlog ("FPGA server", 0, LOG_DAEMON);
#endif /* BE_DAEMON */

	/* TO DO: Process command-line options */

	rc = coordinator_body ();
	exit ( rc ? EXIT_FAILURE : EXIT_SUCCESS );
}
