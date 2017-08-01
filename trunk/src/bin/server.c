/*
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * ––––––––––––––––––––––––––––––––––– API ––––––––––––––––––––––––––––––––––––
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * Format for valid save requests is:
 *   Frame 1: Filename
 *   Frame 2: No. of ticks (NOTE it's interpreted as unsigned int; if = 0, only
 *            a signle frame is saved)
 *   Frame 3: 0 (for "create"), 1 (for "create or overwrite"), 2 (for "append")
 *            or 3 (for "status", NOT IMPLEMENTED YET)
 *
 * Format for reply to a valid request is:
 *   Frames 1--3: Original 3 frames
 *   Frame     3: Number of ticks written (may be less than requested if error
 *                occurred or if MAX_FSIZE was reached
 *
 * Format for reply to an invalid request is:
 *   Frames 1--n: Original n frames
 *   Frame  .n+1: "invalid"
 *
 * 
 * Description coming soon
 * 
 *
 *    –––––––––     ––––––––––––––––
 *    |  ???  |     |  DISH  / SUB |                client
 *    –––––––––     ––––––––––––––––
 *         |               |
 *
 * –– save to file –––––– MCA ––––––––––––––––––––––––––––
 *
 *         |               |
 *    ––––––––      –––––––––––––––
 *    | REP  |      | RADIO / PUB |
 *    ––––––––      –––––––––––––––
 * 
 *    ––––––––                                 main thread
 *    | PAIR |
 *    ––––––––
 *         |
 * –––––––––––––––––––––––––––––––––––––––––––––––––––––––
 *         |
 *    ––––––––        –––––––––––––
 *    | PAIR |        | Netmap fd |            FPGA thread
 *    ––––––––        –––––––––––––
 * 
 *
 * At the moment we pre-allocate a maximum file size when opening the file and
 * truncate when closing. If the maximum is reached, we simply stop, do not
 * create a new one or enlarge. Should we handle this differently?
 *
 * At the moment we only handle one save-to-file request at a time.
 *
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * –––––––––––––––––––––––––––––––– DEV NOTES –––––––––––––––––––––––––––––––––
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * We use assert throughout to catch bugs. Normally these statements should
 * never be reached regardless of user input. Other errors are handled
 * gracefully with messages to syslog or stderr/out.
 *
 * There a two threads in the process---the main and the FPGA threads. The main
 * one opens the REP and RADIO/PUB sockets. It then starts the FPGA thread
 * using zactor high-level class. zactor_new creates two PAIR zmq sockets and
 * uses a wrapper around pthread_create, called s_thread_shim (not accessible
 * outside its source code). It starts the actual thread body (which we pass to
 * zactor_new), passing it its end of the pipe (a PAIR socket) as well as
 * a void* argument of our choice (again, given to zactor_new). The main body
 * must signal down the pipe using zsock_signal (doesn't matter the byte
 * status), since zactor_new will be waiting for this before it returns. The
 * handler must listen on the pipe for a terminating signal, which is sent by
 * the actor's destructor (called by zactor_destroy). Upon receiving this
 * signal the handler must return. It returns into s_thread_shim which signals
 * down the pipe before destroying the pipe. The destructor must wait for this
 * signal before returning into zactor_destroy, which destroys the other end of
 * the pipe and returns to the caller. Hence zactor_destroy acts analogously to
 * pthread_cancel + pthread_join. The default destructor sends a single-frame
 * message from the string "$TERM". zactor_set_destructor, which can set
 * a custom destructor is a DRAFT method only available in latest commits, so
 * we stick to the default one for now. But since we want to deal with integer
 * signals and not string messages, we define fpga_destroy as a wrapper around
 * zactor_destroy, which sends SIG_STOP and then calls zactor_destroy.
 *
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * –––––––––––––––––––––––––––––––––– TO DO –––––––––––––––––––––––––––––––––––
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * Send a control signal between the two threads at some regular interval to
 *     make sure they are running ok.
 */

#include "common.h"

/* The following macros change parts of the implementation */
// #define BE_DAEMON
// #define CUSTOM_ZACTOR_DESTRUCTOR

#ifdef BE_DAEMON
#  include "daemon.h"
#else /* BE_DAEMON */
/* common.h uses syslog for ERROR, DEBUG, WARN, INFO */
#  undef ERROR
#  undef WARN
#  undef INFO
#  undef DEBUG
#  define ERROR(...) if (1) {          \
	fprintf (stdout, "Thread %p: ", (void*) pthread_self()); \
	fprintf (stdout, __VA_ARGS__); \
	fprintf (stdout, "\n");        \
	if (errno)                     \
		fputs (strerror (errno), stdout); \
	fflush (stdout);               \
	} else (void)0
#  define WARN(...) if (1) {           \
	fprintf (stdout, "Thread %p: ", (void*) pthread_self()); \
	fprintf (stdout, __VA_ARGS__); \
	fprintf (stdout, "\n");        \
	fflush (stdout);               \
	} else (void)0
#  define INFO(...) if (1) {           \
	fprintf (stdout, "Thread %p: ", (void*) pthread_self()); \
	fprintf (stdout, __VA_ARGS__); \
	fprintf (stdout, "\n");        \
	fflush (stdout);               \
	} else (void)0
#  define DEBUG(...) if (1) {          \
	fprintf (stderr, "Thread %p: ", (void*) pthread_self()); \
	fprintf (stderr, __VA_ARGS__); \
	fprintf (stderr, "\n");        \
	fflush (stderr);               \
	} else (void)0
#endif /* BE_DAEMON */

#define MAX_FSIZE  5ULL << 32 /* 20GB */
#define UPDATE_INTERVAL 2000  /* in milliseconds */

#define SAVEJOB_IF "tcp://lo:5555"
#define PUBLISH_IF "tcp://lo:5556"
#define FPGA_NM_IF "vale:fpga"

/* Signals for communicating between main and FPG thread */
#define SIG_INIT 0 /* FPGA to main thread when ready */
#define SIG_STOP 1 /* Either thread to the other when error */
#define SIG_MCA  2 /* FPGA to main thread when MCA packet is ready */
#define SIG_FILE 3 /* FPGA to main thread when save job is done */

/* Sent to the client if we don't understand a request */
#define REQ_INVAL       "invalid"
#define REQ_FRAMES      5 /* 3 + ID and envelope from router socket */
#define REQ_DONE_FRAMES 6

/* Data related to the currently saved file */
struct sjob_t
{
	char* filename;
	int fd;
	void* map;
	u_int64_t max_ticks;
	u_int64_t size;
	u_int64_t ticks;
};

/*
 * Allocated and cleaned by the main thread, passed to the FPGA thread at
 * creation 
 */
struct pt_data_t
{
	/* fd, map and max_ticks are set by the main thread upon receiving
	 * a request
	 * size and ticks are incremented by the FPGA thread as it writes */
	struct sjob_t cur_sjob;
	/* TO DO: a mapped area to put MCA frames into */
};

/*
 * Data managed by the main thread, includes data passed to the FPGA thread.
 */
struct g_data_t
{
	struct pt_data_t pt_data;
	zsock_t* sjob_rep;
	// zsock_t* mca_pub;
};

static void fpga_stop (zactor_t* self);
static void fpga_destroy (zactor_t** self);
static void finalize_job (struct g_data_t* data);
static void enable_save (struct g_data_t* data);
static int  print_stats (zloop_t* loop, int timer_id, void* stats_);
static int  sjob_req_hn (zloop_t* loop, zsock_t* reader, void* data_);
static int  fpga_to_main_hn (zloop_t* loop, zsock_t* reader, void* data_);
static int  nm_rx_hn (zloop_t* loop, zmq_pollitem_t* pitem, void* pt_data_);
static int  main_to_fpga_sig_hn (zloop_t* loop, zmq_pollitem_t* pitem, void* ignored);
static void fpga_comm_body (zsock_t* pipe, void* pt_data_);

/* ------------------------------- HELPERS --------------------------------- */

#ifdef CUSTOM_ZACTOR_DESTRUCTOR
/*
 * This mirrors the default destructor except that it sends a signal rather
 * than a string message. All of the communication between the main and FPGA
 * thread is using zmq signals. 
 */
static void
fpga_stop (zactor_t* self)
{
	assert (self);
	if (zsock_signal (self, SIG_STOP) == 0)
		zsock_wait (self); /* zactor_new starts a wrapper around the
				    * requested handler which signals when the
				    * handler (fpga_comm_body) returns, so this
				    * is analogous to pthread_join */
}
#endif /* CUSTOM_ZACTOR_DESTRUCTOR */

/*
 * This is to be used instead of zactor_destroy as a workaround for not setting
 * a custom destructor.
 */
static void
fpga_destroy (zactor_t** self)
{
	assert (self);
#ifndef CUSTOM_ZACTOR_DESTRUCTOR
	zsock_signal (*self, SIG_STOP);
#endif /* CUSTOM_ZACTOR_DESTRUCTOR */
	zactor_destroy (self); /* if not using fpga_stop as a custom
				 * destructor, zactor_destroy will send "$TERM"
				 * which will be ignored; not a problem */
}

/*
 * Called by the main thread when a save job is done or when we are shutting
 * down. Will send a message to the client.
 */
static void
finalize_job (struct g_data_t* data)
{
	/* TO DO: Save stats about the job */
	if ( data->pt_data.cur_sjob.map != NULL &&
		data->pt_data.cur_sjob.map != (void*)-1)
	{
		munmap (data->pt_data.cur_sjob.map, MAX_FSIZE);
		data->pt_data.cur_sjob.map = NULL;
	}

	if (data->pt_data.cur_sjob.fd >= 0)
	{
		int rc = ftruncate (data->pt_data.cur_sjob.fd,
			data->pt_data.cur_sjob.size);
		if (rc == -1)
		{
			WARN ("Could not truncate file");
		}
		close (data->pt_data.cur_sjob.fd);
		data->pt_data.cur_sjob.fd = -1;
	}

	/* TO DO: Send message to client */

	zstr_free (&data->pt_data.cur_sjob.filename);
	data->pt_data.cur_sjob.size = 0;
	data->pt_data.cur_sjob.ticks = 0;
	data->pt_data.cur_sjob.max_ticks = 0;
}

/*
 * Called by the main thread when it receives a valid request to save to file.
 * In case of an error it sends a message to the client.
 */
static void
enable_save (struct g_data_t* data)
{
}

/* --------------------------- SOCKET HANDLERS ----------------------------- */

/*
 * When working in foreground, print statistics (bandwidth, etc) every
 * UPDATE_INTERVAL
 */
static int
print_stats (zloop_t* loop, int timer_id, void* stats_)
{
	INFO ("stats");
	return 0;
}

/*
 * Called by the main thread when it receives a request from a client to save
 * to file. Opens the file, mmaps it and sets the maximum ticks to save. The
 * FPGA thread checks this every time it reads a packet and will copy the
 * packet to the file is the map is non-NULL.
 */
static int
sjob_req_hn (zloop_t* loop, zsock_t* reader, void* data_)
{
	struct g_data_t* data = (struct g_data_t*) data_;

	return 0;
}

/*
 * Called by the main thread when it receives a signal from the FPGA thread.
 * This will either be when a full MCA histogram is ready to be sent, when
 * a save job is done or when the FPGA thread encountered an error and is
 * shutting down.
 */
static int
fpga_to_main_hn (zloop_t* loop, zsock_t* reader, void* data_)
{
	struct g_data_t* data = (struct g_data_t*) data_;

	// int sig = zsock_wait (reader);
	/* Catch bugs by receiving a message and asserting it's a signal.
	 * zsock_wait discards messages until a signal arrives. */
	zmsg_t* msg = zmsg_recv (reader);
	if (msg == NULL)
	{
		DEBUG ("Receive interrupted");
		return -1;
	}
	int sig = zmsg_signal (msg);
	zmsg_destroy (&msg);
	assert (sig >= 0);

	switch (sig)
	{
		case SIG_STOP:
			DEBUG ("FPGA thread encountered an error");
			return -1;
		case SIG_MCA:
			DEBUG ("MCA histogram ready, sending out");
			/* TO DO */
			break;
		case SIG_FILE:
			DEBUG ("Save job ready, finalizing");
			finalize_job (data);
			break;
		case SIG_INIT:
			/* This should have been handled before starting the loop. */
			assert (0);
			break;
		default:
			/* We forgot to handle some signal */
			assert (0);
			break;
	}

	return 0;
}

/*
 * Called by the FPGA thread when there are packets in the rx ring. We do all
 * important tasks here.
 */
static int
nm_rx_hn (zloop_t* loop, zmq_pollitem_t* pitem, void* pt_data_)
{
	DEBUG ("Something in the rx ring");
	struct pt_data_t* pt_data = (struct pt_data_t*) pt_data_;

	return 0;
}

/*
 * Called by the FPGA thread when it receives a signal from the main thread.
 * This should only happen in case of an error and we need to exit.
 */
static int
main_to_fpga_sig_hn_2 (zloop_t* loop, zsock_t* reader, void* ignored)
{
	DEBUG ("Got a signal from main thread");
	// int sig = zsock_wait (reader);
	/* Catch bugs by receiving a message and asserting it's a signal.
	 * zsock_wait discards messages until a signal arrives. */
	zmsg_t* msg = zmsg_recv (reader);
	assert (msg);
	int sig = zmsg_signal (msg);
	zmsg_destroy (&msg);
	assert (sig >= 0);

	switch (sig)
	{
		case SIG_STOP:
			DEBUG ("main thread is terminating us");
			return -1;
		default:
			/* We forgot to handle some signal */
			assert (0);
			break;
	}

	return 0;
}

/* --------------------------- THREADS' BODIES ----------------------------- */

/*
 * The main body of the FPGA thread. It is responsible for opening and closing
 * the netmap interface and is the only one that reads from it.
 * It saves MCA frames to a pre-allocated memory area that the main thread can
 * access. When receiving the last part of an MCA frame it signals the main
 * thread and the main thread is responsible for sending it as a packet to the
 * subscribers via its RADIO socket.
 * It also checks if the mapped region passed to it via the argument is valid
 * and if it is it copies ALL frames there, accumulating statistics. When the
 * maximum requested ticks are reached, it signals the main thread. The main
 * thread closes the file and sends a reply to the client via its ROUTER
 * socket.
 * The idea is to have as few tasks in addition to reading from the netmap tx
 * ring, and as few interruptions (signal handling, function calls) as
 * possible.
 */
static void
fpga_comm_body (zsock_t* pipe, void* pt_data_)
{
	zsock_signal (pipe, SIG_INIT); /* zactor_new will wait for this */
	struct pt_data_t* pt_data = (struct pt_data_t*) pt_data_;

	/* Open the interface */
	struct nm_desc* nmd = nm_open(FPGA_NM_IF"}1", NULL, 0, 0);
	if (nmd == NULL)
	{
		ERROR ("Could not open interface %s", FPGA_NM_IF);
		zsock_signal (pipe, SIG_STOP); /* the main thread will
						     * wait for this */
		return;
	}
	INFO ("Opened interface %s", FPGA_NM_IF"}1");

	/* Get the ring (we only use one) */
	assert (nmd->first_rx_ring == nmd->last_rx_ring);
	struct netmap_ring* rxring = NETMAP_RXRING ( nmd->nifp,
		nmd->first_rx_ring);

	/* Register the pollers, we use the lower-level zloop_poller in order
	 * to handle the netmap file descriptor as well */
	struct zmq_pollitem_t pitem;
	pitem.fd = nmd->fd;
	pitem.events = ZMQ_POLLIN;
	zloop_t* loop = zloop_new ();
	zloop_set_nonstop (loop, 1); /* only the main thread should get
				      * interrupted, we wait for SIG_STOP */
	zloop_poller (loop, &pitem, nm_rx_hn, pt_data);
	zloop_reader (loop, pipe, main_to_fpga_sig_hn_2, NULL);

	zsock_signal (pipe, SIG_INIT); /* the main thread will
					     * wait for this */

	DEBUG ("Starting netmap poll");
	int rc = zloop_start (loop);
	assert (rc == -1);

	/* We don't signal the main thread here, since we may be exiting after
	 * receiving the signal from zactor_destroy, in which case the main
	 * thread will be waiting for the signal from s_thread_shim (the
	 * wrapper).
	 * If we exited due to an error on our part (in nm_rx_hn), the
	 * SIG_STOP would have been sent there, prior to the handler
	 * returning -1 and terminating the loop. In that case, the main thread
	 * will call zactor_destroy to collect the final signal from
	 * s_thread_shim.
	 */
	nm_close (nmd);
	zloop_destroy (&loop);
	DEBUG ("Done");
}

int
main (void)
{
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

	struct g_data_t data;
	memset (&data, 0, sizeof (data));
	data.pt_data.cur_sjob.fd = -1;
	/* TO DO: a mapped area to put MCA frames into */

	/* Start the thread, will block until fpga_comm_body signals */
	zactor_t* fpga_comm = zactor_new (fpga_comm_body, &data.pt_data);
#ifdef CUSTOM_ZACTOR_DESTRUCTOR
	zactor_set_destructor (fpga_comm, fpga_stop);
#endif /* CUSTOM_ZACTOR_DESTRUCTOR */
	/* zactor_new does not check the signal, so no way to know if there was
	 * an error. As a workaroung the FPGA thread will send a second signal
	 * when it is ready (or when it fails) and we wait for it here. */
	rc = zsock_wait (fpga_comm);
	if (rc == SIG_STOP)
	{
		DEBUG ("FPGA thread failed to initialize");
		fpga_destroy (&fpga_comm); /* destroy the pipe */
		exit (EXIT_FAILURE);
	}
	assert (rc == SIG_INIT);
	DEBUG ("FPGA thread initialized");

	/* Open the readers for the clients */
	data.sjob_rep = zsock_new_rep ("@"SAVEJOB_IF);
	if (data.sjob_rep == NULL)
	{
		ERROR ("Could not open the sockets");
		fpga_destroy (&fpga_comm);
		exit (EXIT_FAILURE);
	}
	DEBUG ("Opened the REP interface");

	/* Register the readers */
	zloop_t* loop = zloop_new ();
	rc  = zloop_reader (loop, data.sjob_rep, sjob_req_hn, &data);
	rc |= zloop_reader (loop, zactor_sock (fpga_comm), fpga_to_main_hn,
		&data);
	if (rc)
	{
		ERROR ("Could not register the readers");
		zsock_destroy (&data.sjob_rep);
		zloop_destroy (&loop);
		fpga_destroy (&fpga_comm);
		exit (EXIT_FAILURE);
	}

#ifndef BE_DAEMON
	/* Set the timer */
	rc = zloop_timer (loop, UPDATE_INTERVAL, 0, print_stats, NULL);
	if (rc == -1)
	{
		ERROR ("Could not register a timer");
		zsock_destroy (&data.sjob_rep);
		zloop_destroy (&loop);
		fpga_destroy (&fpga_comm);
		exit (EXIT_FAILURE);
	}
	DEBUG ("Will print stats every %d milliseconds", UPDATE_INTERVAL);
#endif /* BE_DAEMON */

	INFO ("Waiting for jobs");
	rc = zloop_start (loop);

	DEBUG (rc ? "Terminated by handler" : "Interrupted");

	finalize_job (&data);
	zsock_destroy (&data.sjob_rep);
	zloop_destroy (&loop);
	fpga_destroy (&fpga_comm);
	exit ( rc ? EXIT_FAILURE : EXIT_SUCCESS );
}
