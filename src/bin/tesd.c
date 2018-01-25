/*
 * Also see README for the API.
 *
 *      -------           -------         -------    
 *      | REQ |           | SUB |         | REQ |           client
 *      -------           -------         -------    
 *         |                 |               |       
 *                                                     
 * --- save to file ---- histogram ---- average trace ------------
 *                                                     
 *         |                 |               |       
 *      -------           -------         -------     
 *      | REP |           | PUB |         | REP |     
 *      -------           -------         -------     
 *                                                    
 *      --------          --------        --------    
 *      | PAIR |          | PAIR |        | PAIR |    
 *      --------          --------        --------         server
 *         |                 |               |        
 *      ------------ task coordinator ------------
 *         |                 |               |        
 *      --------          --------        --------    
 *      | PAIR |          | PAIR |        | PAIR |            
 *      --------          --------        --------    
 * 
 * 
 *
 * ----------------------------------------------------------------------------
 * -------------------------------- DEV NOTES ---------------------------------
 * ----------------------------------------------------------------------------
 * When debugging we use assert throughout to catch bugs. Normally these
 * statements should never be reached regardless of user input. Other errors
 * are handled gracefully with messages to clients and/or syslog or stderr/out.
 * dbg_assert is used in functions which are called very often (e.g. handlers)
 * and is a no-op unless ENABLE_DEBUG is defined.
 *
 * There is a separate thread for each "task" (see tesd_tasks.c). Tasks are
 * started with tasks_start. Each task has read-only access to rings (they
 * cannot modify the cursor or head) and each task keeps its own head, which is
 * visible by the coordinator (tesd.c).
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
 * signal handler is generic, internal to tesd_tasks. Coordinator simply passes
 * the loop to tasks_start and after exiting from its loop (for whatever reason)
 * calls tasks_stop to shutdown all tasks cleanly.
 *
 * Note: bool type and true/false macros are ensured by CZMQ.
 *
 * ----------------------------------------------------------------------------
 * ---------------------------------- TO DO -----------------------------------
 * ----------------------------------------------------------------------------
 * - chroot and drop privileges.
 */

#include "tesd.h" // common to tasks and coordinator
#include "tesd_tasks_coordinator.h" // declarations of external task-related functions
#include "net/tesif_manager.h"
#include <sys/time.h>
#include <syslog.h>
#include <signal.h>
#include <poll.h>
#include <net/if.h> /* IFNAMSIZ */
#include <sys/socket.h>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <net/if.h>

#ifdef linux
#  define ifr_index ifr_ifindex
#  define sockaddr_dl sockaddr_ll
#  define sdl_family sll_family 
#  define sdl_index sll_ifindex
#  define IFNAME "eth0"
#else
#  define IFNAME "igb1"
#endif

#define NEED_PROMISC // put the interface in promiscuous mode

#define PROGNAME "tesd"

/* Defaults */
#define UPDATE_INTERVAL 1 // in seconds
#define TES_IFNAME "netmap:" IFNAME
#define PIDFILE "/var/run/" PROGNAME ".pid"

/*
 * Statistics, only used in foreground mode
 */
struct stats_accumulated_t
{
	uint64_t received;
	uint64_t missed;
	uint64_t polled;
	uint64_t skipped;
};

struct stats_t
{
	struct timeval last_update;
	struct stats_accumulated_t latest;
	struct stats_accumulated_t total;
};

struct data_t
{
	struct stats_t stats;
	tes_ifdesc* ifd;
};

static void s_usage (const char* self);
static int  s_prepare_if (const char* ifname_full);
static int  s_print_stats (zloop_t* loop, int timer_id, void* stats_);
static int  s_new_pkts_hn (zloop_t* loop, zmq_pollitem_t* pitem, void* data_);
static int  s_coordinator_body (const char* ifname, long int stat_period);

/* ------------------------------------------------------------------------- */

static void
s_usage (const char* self)
{
	fprintf (stderr,
		"Usage: %s [options]\n\n"
		"Options:\n"
		"    -p <file>            Write pid to file <file>.\n"
		"                         Only in daemon mode.\n"
		"                         Defaults to "PIDFILE"\n"
		"    -i <if>              Read packets from <if> interface\n"
		"                         Defaults to "TES_IFNAME"\n"
		"    -f                   Run in foreground\n"
		"    -u <n>               Print statistics every <n> seconds\n"
		"                         Set to 0 to disable. Default is %d\n"
		"                         in foreground and 0 in daemon mode\n"
		"    -v                   Print debugging messages\n",
		self, UPDATE_INTERVAL
		);
	exit (EXIT_FAILURE);
}

/*
 * Bring the interface up and put it in promiscuous mode.
 */
static int
s_prepare_if (const char* ifname_full)
{
	int rc;

	/* Parse the name, extract only the physical interface name. */
	/* Vale ports don't need to even be up. */
	if (memcmp (ifname_full, "vale", 4) == 0)
		return 0;

	char ifname[IFNAMSIZ];
	memset (ifname, 0, IFNAMSIZ);

	/* Skip over optional "netmap:" (or anything else?). */
	const char* start = strchr (ifname_full, ':');
	if (start == NULL)
		start = ifname_full;
	else
		start++;

	/* Find the start of any of the special suffixes. */
	const char* end = ifname_full;
	for (; *end != '\0' &&
		(strchr("+-*^{}/@", *end) == NULL); end++)
		;
	if (end <= start)
	{
		logmsg (0, LOG_ERR,
			"Malformed interface name '%s'", ifname_full);
		return -1;
	}

	snprintf (ifname, end - start + 1, "%s", start);
	dbg_assert (strlen (ifname) == (size_t)(end - start));

	/* A socket is needed for ioctl. */
	int sock = socket (AF_INET, SOCK_DGRAM, htons (IPPROTO_IP));
	if (sock == -1)
	{
		logmsg (errno, LOG_ERR,
			"Could not create a raw socket");
		return -1;
	}

	/* Retrieve the index of the interface. */
	struct ifreq ifr;
	memset (&ifr, 0, sizeof (ifr));
	strcpy (ifr.ifr_name, ifname);
	rc = ioctl (sock, SIOCGIFINDEX, &ifr);
	if (rc == -1)
	{
		logmsg (errno, LOG_ERR,
			"Could not get the interface's index");
		return -1;
	}

	/* Bring the interface up. */
	rc = ioctl (sock, SIOCGIFFLAGS, &ifr);
	if (rc == -1)
	{
		logmsg (errno, LOG_ERR,
			"Could not get the interface's state");
		return -1;
	}
	if (! (ifr.ifr_flags & IFF_UP))
	{
		ifr.ifr_flags = IFF_UP;
		rc = ioctl (sock, SIOCSIFFLAGS, &ifr);
		if (rc == -1)
		{
			logmsg (errno, LOG_ERR,
				"Could not bring the interface up");
			return -1;
		}
		/* check */
		rc = ioctl (sock, SIOCGIFFLAGS, &ifr);
		if (rc == -1)
		{
			logmsg (errno, LOG_ERR,
				"Could not get the interface's state");
			return -1;
		}
		if (! (ifr.ifr_flags & IFF_UP))
		{
			logmsg (errno, LOG_ERR,
				"Could not bring the interface up");
			return -1;
		}
	}
	logmsg (0, LOG_DEBUG, "Interface is up");

#ifdef NEED_PROMISC
	/* Put the interface in promiscuous mode. */
	if (! (ifr.ifr_flags & IFF_PROMISC))
	{
#ifdef linux
		ifr.ifr_flags |= IFF_PROMISC;
#else
		ifr.ifr_flags &= ~IFF_PROMISC;
		ifr.ifr_flagshigh |= IFF_PPROMISC >> 16;
#endif
		rc = ioctl (sock, SIOCSIFFLAGS, &ifr);
		if (rc == -1)
		{
			logmsg (errno, LOG_ERR,
				"Could not put the interface in promiscuous mode");
			return -1;
		}
		/* check */
		rc = ioctl (sock, SIOCGIFFLAGS, &ifr);
		if (rc == -1)
		{
			logmsg (errno, LOG_ERR,
				"Could not get the interface's state");
			return -1;
		}
		if (! (ifr.ifr_flags & IFF_PROMISC))
		{
			logmsg (errno, LOG_ERR,
				"Could not put the interface in promiscuous mode");
			return -1;
		}
	}
	logmsg (0, LOG_DEBUG, "Interface is in promiscuous mode");
#endif /* NEED_PROMISC */

	return 0;
}
/*
 * Print statistics (bandwidth, etc).
 */
static int
s_print_stats (zloop_t* loop, int timer_id, void* stats_)
{
	dbg_assert (stats_ != NULL);
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

	stats->total.received += stats->latest.received;
	stats->total.missed   += stats->latest.missed;
	stats->total.polled   += stats->latest.polled;
	stats->total.skipped  += stats->latest.skipped;
	
	if (loop == NULL)
	{ /* final stats, exiting */
		logmsg (0, LOG_INFO, 
			"received: %10lu   | "
			"missed: %10lu   | "
			"polled: %10lu   | "
			"skipped polls: %10lu   | ",
			stats->total.received,
			stats->total.missed,
			stats->total.polled,
			stats->total.skipped
		       );
	}
	else
	{ /* called by zloop's timer */
		logmsg (0, LOG_INFO, 
			// "elapsed: %2.5fs   | "
			"missed: %10lu   | "
			"skipped polls: %10lu   | "
			"avg pkts per poll: %10lu   | "
			"avg bandwidth: %10.3e pps",
			// tdelta,
			stats->latest.missed,
			stats->latest.skipped,
			(stats->latest.polled) ?
			stats->latest.received / stats->latest.polled : 0,
			(double) stats->latest.received / tdelta
		       );
	}

	memcpy (&stats->last_update, &tnow, sizeof (struct timeval));
	stats->latest.received = 0;
	stats->latest.missed   = 0;
	stats->latest.polled   = 0;
	stats->latest.skipped  = 0;

#if 0
	if ( ! is_daemon () )
	{
		fflush (stdout);
		fflush (stderr);
	}
#endif

	return 0;
}

/*
 * Called when new packets arrive in the ring.
 */
static int
s_new_pkts_hn (zloop_t* loop, zmq_pollitem_t* pitem, void* data_)
{
	dbg_assert (data_ != NULL);
	struct data_t* data = (struct data_t*) data_;

	/* For each ring get the head of the slowest task. */
	uint32_t* heads = tasks_get_heads ();
	/* FIX: check if NULL */

	/* Signal the waiting tasks. */
	int rc = tasks_wakeup ();
	if (rc)
	{
		logmsg (0, LOG_DEBUG,
			"Could not wake up all waiting tasks.");
		return -1;
	}

	/* Save statistics. */
	data->stats.latest.polled++;
	int skipped = 1;
	for (int r = 0; r < NUM_RINGS; r++)
	{
		tes_ifring* rxring = tes_if_rxring (data->ifd, r);
		if (tes_ifring_tail (rxring) == tes_ifring_head (rxring))
			continue; /* nothing in this ring */

		uint32_t new_head;
		if (heads == NULL)
			new_head = tes_ifring_tail (rxring);
		else
			new_head = heads[r];

		if (new_head == tes_ifring_head (rxring))
			continue; /* nothing processed since last time */
		skipped = 0;

		tespkt* pkt = (tespkt*)
			tes_ifring_cur_buf (rxring); /* old head */
		dbg_assert (pkt != NULL);
		uint16_t fseqA = tespkt_fseq (pkt);

		/*
		 * Look at the packet preceding the new head, in case the
		 * new head is the tail (not a valid userspace buffer)
		 */
		pkt = (tespkt*)
			tes_ifring_preceding_buf (rxring, new_head);
		dbg_assert (pkt != NULL);
		uint16_t fseqB = tespkt_fseq (pkt);

		tes_ifring_goto_buf (rxring, new_head); /* cursor -> new head */
		dbg_assert (tes_ifring_cur (rxring) == new_head);
		uint32_t num_new = tes_ifring_done (rxring); /* cursor - old head */

		data->stats.latest.received += num_new;
		data->stats.latest.missed += (uint16_t)(fseqB - fseqA - num_new + 1);

		tes_ifring_release_done_buf (rxring); /* head -> new head */
		dbg_assert (tes_ifring_head (rxring) == tes_ifring_cur (rxring));
	}

	if (skipped)
		data->stats.latest.skipped++;

	return 0;
}

static int
s_coordinator_body (const char* ifname_full, long int stat_period)
{
	char log_id[16];
	snprintf (log_id, sizeof (log_id), "[Coordinator] ");
	set_logid (log_id);

	int rc;
	struct data_t data;
	memset (&data, 0, sizeof (data));

	/*
	 * (struct nm_desc).nifp->ni_name contains the true name as opened,
	 * e.g. if the interface is a persistent vale port, it will contain
	 * vale*:<port> even if nm_open was passed netmap:<port>. (struct
	 * nm_desc).req.nr_name contains the name of the interface passed to
	 * nm_open minus the ring specification and minus optional netmap:
	 * prefix, even if interface is a vale port. So we first open it and
	 * then pass nifp->ni_name to s_prepare_if.
	 */
	/* Open the interface. */
	data.ifd = tes_if_open (ifname_full, NULL, 0, 0);
	if (data.ifd == NULL)
	{
		logmsg (errno, LOG_ERR, "Could not open interface %s",
			ifname_full);
		return -1;
	}
	const char* ifname = tes_if_name (data.ifd);
	dbg_assert (ifname != NULL);
	logmsg (0, LOG_INFO, "Opened interface %s", ifname);
	dbg_assert (tes_if_rxrings (data.ifd) == NUM_RINGS);

	/* Bring the interface up and put it in promiscuous mode. */
	rc = s_prepare_if (ifname);
	if (rc == -1)
		goto cleanup; /* s_prepare_if will print the error */


	/* Start the tasks and register the readers. Tasks are initialized as
	 * inactive. */
	zloop_t* loop = zloop_new ();
	rc = tasks_start (data.ifd, loop);
	if (rc)
	{
		logmsg (0, LOG_DEBUG, "Tasks failed to start");
		goto cleanup;
	}

	/* Register the TES interface as a poller. */
	struct zmq_pollitem_t pitem;
	memset (&pitem, 0, sizeof (pitem));
	pitem.fd = tes_if_fd (data.ifd);
	pitem.events = ZMQ_POLLIN;
	rc = zloop_poller (loop, &pitem, s_new_pkts_hn, &data);
	if (rc == -1)
	{
		logmsg (errno, LOG_ERR,
			"Could not register the zloop poller");
		goto cleanup;
	}

	if (stat_period > 0)
	{
		/* Set the timer. */
		rc = zloop_timer (loop, 1000 * stat_period, 0,
			s_print_stats, &data.stats);
		if (rc == -1)
		{
			logmsg (errno, LOG_ERR, "Could not set a timer");
			goto cleanup;
		}
		logmsg (0, LOG_DEBUG, "Will print stats every %d seconds",
			stat_period);
	}

	logmsg (0, LOG_DEBUG, "All threads initialized");
	rc = zloop_start (loop);

	if (rc)
	{
		logmsg (0, LOG_DEBUG, "Terminated by handler");
	}
	else
	{
		logmsg (0, LOG_DEBUG, "Interrupted");
	}

cleanup:
	tasks_destroy ();
	zloop_destroy (&loop);
	tes_if_close (data.ifd);
	s_print_stats (NULL, 0, &data.stats);
	return rc;
}

int
main (int argc, char **argv)
{
#ifdef ENABLE_FULL_DEBUG
	tespkt_self_test ();
#endif
	int rc;

	/* Process command-line options. */
	bool is_daemon = 1;
	bool is_verbose = 0;
	int opt;
	char* buf = NULL;
	long int stat_period = -1;
	char ifname_full[IFNAMSIZ];
	memset (ifname_full, 0, sizeof (ifname_full));
	char pidfile[PATH_MAX];
	memset (pidfile, 0, sizeof (pidfile));
	while ( (opt = getopt (argc, argv, "p:i:u:fvh")) != -1 )
	{
		switch (opt)
		{
			case 'p':
				snprintf (pidfile, sizeof (pidfile),
					"%s", optarg);
				break;
			case 'i':
				snprintf (ifname_full, sizeof (ifname_full),
					"%s", optarg);
				break;
			case 'u':
				stat_period = strtol (optarg, &buf, 10);
				if (strlen (buf))
				{
					s_usage (argv[0]);
				}
				break;
			case 'f':
				is_daemon = 0;
				break;
			case 'v':
				is_verbose = 1;
				break;
			case 'h':
			case '?':
				s_usage (argv[0]);
				break;
			default:
				/* we forgot to handle an option */
				assert (0);
		}
	}
	if (strlen (pidfile) == 0)
	{
		sprintf (pidfile, PIDFILE);
	}
	if (strlen (ifname_full) == 0)
	{
		sprintf (ifname_full, TES_IFNAME);
	}
	if (stat_period == -1 && ! is_daemon)
	{
		stat_period = UPDATE_INTERVAL;
	}

	if (is_daemon)
	{
		/* Go into background. */
		rc = daemonize (pidfile, NULL, NULL, 0);
		if (rc == -1)
		{
			logmsg (errno, LOG_ERR,
				"Failed to go into background");
			exit (EXIT_FAILURE);
		}
		set_verbose (is_verbose);

		/* Start syslog. */
		openlog ("TES server", 0, LOG_DAEMON);

		logmsg (0, LOG_DEBUG,
			"Wrote pid to file '%s'", pidfile);
	}

	rc = s_coordinator_body (ifname_full, stat_period);

	/* Should we remove the pidfile? */
	logmsg (0, LOG_INFO, "Shutting down");

	exit ( rc ? EXIT_FAILURE : EXIT_SUCCESS );
}
