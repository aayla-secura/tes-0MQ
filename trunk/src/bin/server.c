/*
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * ––––––––––––––––––––––––––––––––––– API ––––––––––––––––––––––––––––––––––––
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * Messages are sent and read via zsock_send, zsock_recv as "picture" messages.
 * Valid requests have a picture of "s81", replies have a picture of "18888":
 * 
 * Format for valid save requests is:
 *   Frame 1 (char*):
 *       Filename
 *   Frame 2 (uint64_t):
 *       0: client requests status of the file (reply will be as described
 *            below) or
 *       N > 1: no. of ticks (NOTE it's read as unsigned int)
 *   Frame 3 (uint8_t): (ignored if Frame 2 == 0)
 *       0: create but do not overwrite or
 *       1: create or overwrite,
 * As a consequence of how zsock_recv parses arguments, the client may omit
 * frames corresponding to ignored arguments or arguments = 0. Therefore to get
 * a status of a file, only the filename is required.
 *
 * Format for a reply to invalid request or an error
 *   Frame 1 (uint8_t):
 *       0: request was invalid or
 *          there was an error opening the file (in case of request for
 *            overwrite) or
 *          file exists (in case of request for create but not overwrite) or
 *          file does not exist (in case of request for status)
 *   Frames 2-5:
 *    	 0: ignore
 *
 * Format for a reply to valid request and no error
 *   Frame 1 (uint8_t):
 *       1: OK
 *   Frame 2 (uint64_t):
 *       N: no. of ticks written (may be less than requested if error occurred or
 *            if MAX_FSIZE was reached)
 *   Frame 3 (uint64_t):
 *       N: no. of bytes written
 *   Frame 4 (uint64_t):
 *       N: no. of frames written
 *   Frame 5 (uint64_t):
 *       N: no. of dropped frames
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
 * At the moment we only handle one save-to-file request at a time. Will block
 * until it is done.
 *
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * –––––––––––––––––––––––––––––––– DEV NOTES –––––––––––––––––––––––––––––––––
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * We use assert throughout to catch bugs. Normally these statements should
 * never be reached regardless of user input. Other errors are handled
 * gracefully with messages to syslog or stderr/out.
 *
 * There a two threads in the process---the main and the FPGA threads.
 *
 * The main one opens the REP and RADIO/PUB sockets.
 * It allocates a pt_data_t structure which contains data to be written to by
 * one thread, read by the other one. It then starts the FPGA thread using
 * zactor high-level class, which on UNIX systems is a wrapper around
 * pthread_create. zactor_new creates two PAIR zmq sockets and creates
 * a detached thread caliing a wrapper (s_thread_shim) around the hanlder of
 * our choice. It starts the actual handler (which we pass to zactor_new),
 * passing it its end of the pipe (a PAIR socket) as well as a void* argument
 * of our choice (again, given to zactor_new). The handler must signal down the
 * pipe using zsock_signal (doesn't matter the byte status), since zactor_new
 * will be waiting for this before it returns. The handler must listen on the
 * pipe for a terminating signal, which is sent by the actor's destructor
 * (called by zactor_destroy). Upon receiving this signal the handler must
 * return. It returns into s_thread_shim which signals down the pipe before
 * destroying that end of the pipe. The destructor must wait for this signal
 * before returning into zactor_destroy, which destroys the other end of the
 * pipe and returns to the caller. Hence zactor_destroy acts analogously to
 * pthread_cancel + pthread_join (for joinable threads). The default destructor
 * sends a single-frame message from the string "$TERM". zactor_set_destructor,
 * which can set a custom destructor is a DRAFT method only available in latest
 * commits, so we stick to the default one for now. But since we want to deal
 * with integer signals, and not string messages, we define fpga_destroy as
 * a wrapper around zactor_destroy, which sends SIG_STOP and then calls
 * zactor_destroy to wait for the handler to return.
 *
 * The main thread polls the client readers (REP and RADIO/PUB) as well as its
 * end of the pipe (a PAIR socket) to the FPGA thread.
 * When it receives a request from a client (on the REP socket) it examines it.
 * If the request is invalid, it replies with error. If it is a valid request
 * for status, it opens the filename and sends the status reply.
 * If it is a valid request to write, it opens the file, mmaps it to a location
 * that is accessible by the FPGA thread and sets to non-zero an integer
 * parameter, which tells the FPGA to copy every received packet to the mmapped
 * region until the maximum ticks are reached. It then disables polling on the
 * REP socket until it receives a SIG_FILE from the FPGA thread (on the PAIR
 * socket). It then writes accumulated statistics to the beginning of the file,
 * closes it, sends the reply to the client and re-enables polling on the REP
 * socket.
 * (NOT IMPLEMENTED YET) When it receives a SIG_MCA from the FPGA thread it
 * reads in the joined MCA frames that the FPGA has been saving to a pre-mapped
 * location and sends them as a single ZMQ message. 
 *
 * The FPGA thread is responsible for opening and closing the netmap interface
 * and is the only one that reads from it.
 * It polls the netmap file descriptor as well as its PAIR socket connected to
 * the main thread.
 * The handler for the netmap file descriptor saves MCA frames to
 * a pre-allocated memory area that the main thread can access. When receiving
 * the last part of an MCA frame it signals the main thread and the main thread
 * is responsible for sending it as a packet to the subscribers via its
 * RADIO/PUB socket.
 * For each packet it also checks an integer parameter (set to non-zero by the
 * main thread after opening a file for writing) and if it is non-zero it
 * copies the packet to the mmapped region, accumulating statistics. When the
 * maximum requested ticks are reached, it sets the parameter to zero and
 * signals the main thread. The main thread closes the file and sends a reply
 * to the client via its REP socket.
 * When receiving a SIG_STOP signal from the main thread on the PAIR socket it
 * exits, cleaning up.
 *
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * –––––––––––––––––––––––––––––––––– TO DO –––––––––––––––––––––––––––––––––––
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * Send a control signal between the two threads at some regular interval to
 *     make sure they are running ok.
 */

#define VERBOSE
#define MULTITHREAD /* this is only used in the debuggind macros in common.h */
/* The following macros change parts of the implementation */
#define USE_MMAP
// #define BE_DAEMON
// #define CUSTOM_ZACTOR_DESTRUCTOR

#ifdef BE_DAEMON
#  define SYSLOG /* print to syslog */
#  include "daemon.h"
#else /* BE_DAEMON */
#  define UPDATE_INTERVAL 2000  /* in milliseconds */
#endif /* BE_DAEMON */

#include "common.h"

#define MAX_FSIZE  5ULL << 32 /* 20GB */
#define FDATA_OFF  32 /* 32 bytes for job statistics, see struct sjob_t */

#define SAVEJOB_IF "tcp://*:55555"
#define PUBLISH_IF "tcp://*:55556"
#define FPGA_NM_IF "vale:fpga"

/* Signals for communicating between main and FPG thread */
#define SIG_INIT 0 /* FPGA to main thread when ready */
#define SIG_STOP 1 /* Either thread to the other when error */
#define SIG_MCA  2 /* FPGA to main thread when MCA packet is ready */
#define SIG_FILE 3 /* FPGA to main thread when save job is done */

/* Sent to the client if we don't understand a request */
#define REQ_FAIL        0
#define REQ_OK          1
#define REQ_PIC      "s81"
#define REP_PIC    "18888"

#ifndef BE_DAEMON
/*
 * Global stats, only used in foreground mode
 */
struct stats_t
{
	u_int64_t rcvd;
	u_int64_t last_rcvd;
	u_int64_t missed;
	u_int64_t dispatched;
	struct timeval last_check;
};
#endif /* BE_DAEMON */

/*
 * Data related to the currently saved file
 */
struct sjob_stats_t
{/* statistics saved to the file; we declare it as a separate struct to avoid
  * potential bugs when changing the layout of sjob_t structure or when reading
  * into it from the opened file */
	u_int64_t ticks;
	u_int64_t size; /* do not exceed MAX_FSIZE */
	u_int64_t frames;
	u_int64_t missed_frames; /* dropped frames */
};

struct sjob_t
{
	/* the following are incremented by the FPGA thread as it saves */
	struct sjob_stats_t st;
	/* the following are set by the main thread before enabling the save
	 * parameter */
	u_int64_t max_ticks;
	char* filename;
#ifdef USE_MMAP
	void* map;
#endif /* USE_MMAP */
	int fd;
	int needs_trunc; /* when opening file for writing, we pre-allocate
			  * space and truncate when closing; not done in read
			  * mode (status requests) */
};

/*
 * Allocated and cleaned by the main thread, passed to the FPGA thread at
 * creation 
 */
struct pt_data_t
{
	zsock_t* sigpipe; /* the loop's readers may need to singnal the main
			   * thread */
	struct netmap_ring* rxring; /* we only use one */
#ifndef BE_DAEMON
	struct stats_t stats;
#endif /* BE_DAEMON */
	struct sjob_t cur_sjob;
	int save_enabled;    /* if non-zero, save to the file in cur_sjob */
	u_int16_t cur_frame; /* keep track of missed frames */
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
static int  sjob_init (struct sjob_t* sjob, mode_t fmode);
static void sjob_read_stat (struct sjob_t* sjob);
static void sjob_write_stat (struct sjob_t* sjob);
static void sjob_close (struct sjob_t* sjob);
#ifndef BE_DAEMON
static int  print_stats (zloop_t* loop, int timer_id, void* stats_);
#endif /* BE_DAEMON */
static int  sjob_req_hn (zloop_t* loop, zsock_t* reader, void* pt_data_);
static int  fpga_to_main_hn (zloop_t* loop, zsock_t* reader, void* data_);
static int  nm_rx_hn (zloop_t* loop, zmq_pollitem_t* pitem, void* pt_data_);
static int  main_to_fpga_sig_hn (zloop_t* loop, zsock_t* reader, void* ignored);
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
	{
		zsock_wait (self); /* zactor_new starts a wrapper around the
				    * requested handler which signals when the
				    * handler (fpga_comm_body) returns, so this
				    * is analogous to pthread_join */
	}
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
	assert (data);
	assert (data->pt_data.save_enabled == 0);
	if (data->pt_data.cur_sjob.fd == -1)
	{
		return;
	}

	/* Send message to client */
	zsock_send (data->sjob_rep, REP_PIC, REQ_OK, 
		data->pt_data.cur_sjob.st.ticks,
		data->pt_data.cur_sjob.st.size,
		data->pt_data.cur_sjob.st.frames,
		data->pt_data.cur_sjob.st.missed_frames);

	sjob_write_stat (&data->pt_data.cur_sjob);
	sjob_close (&data->pt_data.cur_sjob);
}

/*
 * Called by the main thread when it receives a valid request to save to file.
 * In case of an error it returns -1 and the caller should send a message to
 * the client.
 */
static int
sjob_init (struct sjob_t* sjob, mode_t fmode)
{
	assert (sjob);
	assert (sjob->st.ticks == 0);
	assert (sjob->st.size == 0);
	assert (sjob->st.frames == 0);
	assert (sjob->st.missed_frames == 0);
	assert (sjob->filename != NULL);
#ifdef USE_MMAP
	assert (sjob->map == NULL);
#endif /* USE_MMAP */
	assert (sjob->fd == -1);
	assert (sjob->needs_trunc == 0);

	/* Open the file */
	sjob->fd = open (sjob->filename, fmode,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (sjob->fd == -1)
	{
		 /* do not print warning, maybe request was for status */
		return -1;
	}

	int rc;
	if (fmode & (O_RDWR | O_WRONLY))
	{
		sjob->needs_trunc = 1;
		rc = posix_fallocate (sjob->fd, 0, MAX_FSIZE);
		if (rc)
		{
			WARN ("Could not allocate %llu to file", MAX_FSIZE);
			errno = rc; /* posix_fallocate does not set errno */
			return -1;
		}
	}

#ifdef USE_MMAP
	/* if (fmode & O_RDONLY) */
	/* { */
	/*         sjob->map = mmap (NULL, MAX_FSIZE, PROT_READ, */
	/*                 MAP_PRIVATE, sjob->fd, 0); */
	/* } */
	/* else */
	/* { */
	/*         sjob->map = mmap (NULL, MAX_FSIZE, PROT_WRITE, */
	/*                 MAP_SHARED, sjob->fd, 0); */
	/* } */
	sjob->map = mmap (NULL, MAX_FSIZE, PROT_WRITE, MAP_SHARED, sjob->fd, 0);
	if (sjob->map == (void*)-1)
	{
		WARN ("Could not mmap file %s", sjob->filename);
		return -1;
	}
#else /* USE_MMAP */
	rc = lseek (sjob->fd, FDATA_OFF, 0);
	if (rc == (off_t)-1 || rc < FDATA_OFF)
	{
		WARN ("Could not adjust file cursor");
		return -1;
	}
#endif /* USE_MMAP */

	return 0;
}

/*
 * Reads stats previously saved to file. Used when client requests a status for
 * filename
 */
static void
sjob_read_stat (struct sjob_t* sjob)
{
	assert (sjob);
	assert (sjob->st.ticks == 0);
	assert (sjob->st.size == 0);
	assert (sjob->st.frames == 0);
	assert (sjob->st.missed_frames == 0);
	assert (sjob->filename != NULL);
#ifdef USE_MMAP
	assert (sjob->map != NULL && sjob->map != (void*)-1);
#endif /* USE_MMAP */
	assert (sjob->fd != -1);

#ifdef USE_MMAP
	memcpy (&sjob->st, sjob->map, FDATA_OFF);
#else /* USE_MMAP */
	int rc = lseek (sjob->fd, 0, 0);
	if (rc)
	{
		WARN ("Could not seek to beginning of file");
		return;
	}
	rc = read (sjob->fd, &sjob->st, FDATA_OFF);
	if (rc < FDATA_OFF)
	{
		WARN ("Could not read from beginning of file");
		return;
	}
#endif /* USE_MMAP */
}

/*
 * Writes stats to a currently open file. Used right before closing it.
 */
static void
sjob_write_stat (struct sjob_t* sjob)
{
	assert (sjob);
	assert (sjob->filename != NULL);
	assert (sjob->needs_trunc); /* should have been done when opening
				     * a file for writing */
#ifdef USE_MMAP
	assert (sjob->map != NULL && sjob->map != (void*)-1);
#endif /* USE_MMAP */
	assert (sjob->fd != -1);

#ifdef USE_MMAP
	memcpy (sjob->map, &sjob->st, FDATA_OFF);
#else /* USE_MMAP */
	int rc = lseek (sjob->fd, 0, 0);
	if (rc)
	{
		WARN ("Could not seek to beginning of file");
		return;
	}
	rc = write (sjob->fd, &sjob->st, FDATA_OFF);
	if (rc < FDATA_OFF)
	{
		WARN ("Could not write to beginning of file");
		return;
	}
#endif /* USE_MMAP */
}

/*
 * Unmaps mmapped file, closes the file descriptor, nullifies and resets stats
 */
static void
sjob_close (struct sjob_t* sjob)
{
	assert (sjob);
#ifdef USE_MMAP
	if ( sjob->map != NULL && sjob->map != (void*)-1)
	{
		munmap (sjob->map, MAX_FSIZE);
		sjob->map = NULL;
	}
#endif /* USE_MMAP */

	if (sjob->fd >= 0)
	{
		if (sjob->needs_trunc)
		{
			int rc = ftruncate (sjob->fd, sjob->st.size);
			if (rc == -1)
			{
				WARN ("Could not truncate file");
			}
		}
		close (sjob->fd);
		sjob->fd = -1;
	}

	zstr_free (&sjob->filename);
	sjob->needs_trunc = 0;
	sjob->max_ticks = 0;
	sjob->st.ticks = 0;
	sjob->st.size = 0;
	sjob->st.frames = 0;
	sjob->st.missed_frames = 0;
}

/* --------------------------- SOCKET HANDLERS ----------------------------- */

#ifndef BE_DAEMON
/*
 * When working in foreground, print statistics (bandwidth, etc) every
 * UPDATE_INTERVAL
 */
static int
print_stats (zloop_t* loop, int timer_id, void* stats_)
{
	assert (stats_);
	INFO ("stats");

	struct stats_t* stats = (struct stats_t*) stats_;

	return 0;
}
#endif /* BE_DAEMON */

/*
 * Called by the main thread when it receives a request from a client to save
 * to file. Opens the file, mmaps it and sets the maximum ticks to save. The
 * FPGA thread checks this every time it reads a packet and will copy the
 * packet to the file is the map is non-NULL.
 */
static int
sjob_req_hn (zloop_t* loop, zsock_t* reader, void* pt_data_)
{
	assert (pt_data_);
	INFO ("Received a save request");

	struct pt_data_t* pt_data = (struct pt_data_t*) pt_data_;
	assert (pt_data->save_enabled == 0);
	u_int8_t job_mode;

	int rc = zsock_recv (reader, REQ_PIC, &pt_data->cur_sjob.filename,
		&pt_data->cur_sjob.max_ticks, &job_mode);
	if (rc == -1)
	{ /* would also return -1 if picture contained a pointer (p) or a null
	   * frame (z) but message received did not match this signature; this
	   * is irrelevant in this case */
		DEBUG ("Receive interrupted");
		return -1;
	}
	if (pt_data->cur_sjob.filename == NULL || job_mode > 1)
	{
		INFO ("Request was malformed");
		zsock_send (reader, REP_PIC, REQ_FAIL, 0, 0, 0, 0);
		return 0;
	}

	mode_t fmode;
	int exp_errno = 0;
	/*
	 * Set the file open mode and act according to the return status of
	 * open and errno (print a warning of errno is unexpected)
	 * Request is for:
	 *   status: open read-only
	 *           - if successful, read in stats and send reply
	 *           - if failed, send reply (expect errno == ENOENT)
	 *   create: create if non-existing
	 *           - if successful, enable save
	 *           - if failed, send reply (expect errno == EEXIST)
	 *   create: create or overwrite
	 *           - if successful, enable save
	 *           - if failed, send reply (this shouldn't happen)
	 */
	if (pt_data->cur_sjob.max_ticks == 0)
	{ /* status */
#ifdef USE_MMAP
		/* why can't we map read-only file even with MAP_PRIVATE? */
		fmode = O_RDWR;
#else /* USE_MMAP */
		fmode = O_RDONLY;
#endif /* USE_MMAP */
		exp_errno = ENOENT;
	}
	else
	{
		fmode = O_RDWR | O_CREAT;
		if (job_mode == 0)
		{ /* do not overwrite */
				fmode |= O_EXCL;
				exp_errno = EEXIST;
		}
	}

	rc = sjob_init (&pt_data->cur_sjob, fmode);
	if (rc == -1)
	{
		if (errno != exp_errno)
		{
			WARN ("Could not open file %s",
				pt_data->cur_sjob.filename);
		}
		zsock_send (reader, REP_PIC, REQ_FAIL, 0, 0, 0, 0);
		sjob_close (&pt_data->cur_sjob);
		return 0;
	}
	INFO ("Opened file %s for %s", pt_data->cur_sjob.filename,
		(fmode & O_RDWR ) ? "writing" : "reading" );

	if (pt_data->cur_sjob.max_ticks == 0)
	{ /* just read in stats and send reply */
		sjob_read_stat (&pt_data->cur_sjob);
		zsock_send (reader, REP_PIC, REQ_OK, 
			pt_data->cur_sjob.st.ticks,
			pt_data->cur_sjob.st.size,
			pt_data->cur_sjob.st.frames,
			pt_data->cur_sjob.st.missed_frames);
		sjob_close (&pt_data->cur_sjob);
		return 0;
	}

	pt_data->save_enabled = 1;

	/* Disable polling on the reader until the job is done */
	zloop_reader_end (loop, reader);

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
	assert (data_);
	struct g_data_t* data = (struct g_data_t*) data_;
	DEBUG ("Got a signal from FPGA thread");

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
			DEBUG ("MCA histogram ready");
			/* TO DO */
			break;
		case SIG_FILE:
			DEBUG ("Save job ready");
			assert (data->pt_data.save_enabled == 0);
			finalize_job (data);
			/* Re-enable polling on the reader */
			int rc  = zloop_reader (loop, data->sjob_rep, sjob_req_hn,
				&data->pt_data);
			if (rc)
			{
				ERROR ("Could not register the REP reader");
				return -1;
			}
			break;
		case SIG_INIT:
			/* This should have been handled before starting the loop. */
			assert (0);
		default:
			/* We forgot to handle some signal */
			assert (0);
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
	struct pt_data_t* pt_data = (struct pt_data_t*) pt_data_;

#ifndef BE_DAEMON
	pt_data->stats.dispatched++;
#endif /* BE_DAEMON */
	do
	{
		u_int32_t cur_bufid =
			pt_data->rxring->slot[ pt_data->rxring->cur ].buf_idx;
		fpga_pkt* pkt =
			(fpga_pkt*) NETMAP_BUF (pt_data->rxring, cur_bufid);

		uint16_t prev_frame = pt_data->cur_frame;
		pt_data->cur_frame = frame_seq (pkt);
#ifndef BE_DAEMON
		if (pt_data->stats.rcvd == 0)
		{
			INFO ("First received frame is #%hu",
				pt_data->cur_frame);
		}
		pt_data->stats.missed += (u_int64_t) (
			(uint16_t)(pt_data->cur_frame - prev_frame) - 1);

		pt_data->stats.rcvd++;
		if (pt_data->stats.rcvd == 0)
		{
			DEBUG ("No. received packets wrapped around");
			pt_data->stats.rcvd++; /* don't reset cur_frame next time */
		}
#endif /* BE_DAEMON */

		if (pt_data->save_enabled)
		{ /* save packet */
#ifdef USE_MMAP
			memcpy ((u_int8_t*)pt_data->cur_sjob.map + FDATA_OFF +
				pt_data->cur_sjob.st.size, pkt, pkt_len (pkt));
#else /* USE_MMAP */
			int rc = write (pt_data->cur_sjob.fd, pkt, pkt_len (pkt));
			if (rc == -1)
			{
				ERROR ("Could not write to file");
				return -1;
			}
#endif /* USE_MMAP */
			if (is_tick (pkt))
				pt_data->cur_sjob.st.ticks++;
			pt_data->cur_sjob.st.size += pkt_len (pkt);
			pt_data->cur_sjob.st.frames++;
			pt_data->cur_sjob.st.missed_frames += (u_int64_t) (
				(uint16_t)(pt_data->cur_frame - prev_frame) - 1);

			if ( ( pt_data->cur_sjob.st.ticks ==
				  pt_data->cur_sjob.max_ticks ) || 
			     ( pt_data->cur_sjob.st.size + MAX_FPGA_FRAME_LEN
				  + FDATA_OFF > MAX_FSIZE ) )
			{
				pt_data->save_enabled = 0;
				zsock_signal (pt_data->sigpipe, SIG_FILE);
			}
		}

		pt_data->rxring->head = pt_data->rxring->cur =
			nm_ring_next(pt_data->rxring, pt_data->rxring->cur);

	} while ( ! nm_ring_empty (pt_data->rxring) );

	return 0;
}

/*
 * Called by the FPGA thread when it receives a signal from the main thread.
 * This should only happen in case of an error and we need to exit.
 */
static int
main_to_fpga_sig_hn (zloop_t* loop, zsock_t* reader, void* ignored)
{
	DEBUG ("Got a signal from main thread");
	// int sig = zsock_wait (reader);
	/* Catch bugs by receiving a message and asserting it's a signal.
	 * zsock_wait discards messages until a signal arrives. */
	zmsg_t* msg = zmsg_recv (reader);
	if (msg == NULL)
	{
		DEBUG ("Receive nterrupted");
		return -1;
	}
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
	}

	return 0;
}

/* --------------------------- THREADS' BODIES ----------------------------- */

/* See comments in DEV NOTES section at the beginning */

static void
fpga_comm_body (zsock_t* pipe, void* pt_data_)
{
	assert (pt_data_);
	zsock_signal (pipe, SIG_INIT); /* zactor_new will wait for this */
	struct pt_data_t* pt_data = (struct pt_data_t*) pt_data_;
	pt_data->sigpipe = pipe;

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
	pt_data->rxring = NETMAP_RXRING ( nmd->nifp,
		nmd->first_rx_ring);

	/* Register the pollers, we use the lower-level zloop_poller in order
	 * to handle the netmap file descriptor as well */
	struct zmq_pollitem_t pitem;
	memset (&pitem, 0, sizeof (pitem));
	pitem.fd = nmd->fd;
	pitem.events = ZMQ_POLLIN;
	zloop_t* loop = zloop_new ();
	zloop_set_nonstop (loop, 1); /* only the main thread should get
				      * interrupted, we wait for SIG_STOP */
	zloop_poller (loop, &pitem, nm_rx_hn, pt_data);
	zloop_reader (loop, pipe, main_to_fpga_sig_hn, NULL);

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
	assert (sizeof (struct sjob_stats_t) == FDATA_OFF);
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
	rc  = zloop_reader (loop, data.sjob_rep, sjob_req_hn, &data.pt_data);
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
	rc = zloop_timer (loop, UPDATE_INTERVAL, 0, print_stats,
		&data.pt_data.stats);
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

	if (rc)
	{
		DEBUG ("Terminated by handler");
	}
	else
	{
		DEBUG ("Interrupted");
	}

	data.pt_data.save_enabled = 0;
	finalize_job (&data);
	zsock_destroy (&data.sjob_rep);
	zloop_destroy (&loop);
	fpga_destroy (&fpga_comm);
	exit ( rc ? EXIT_FAILURE : EXIT_SUCCESS );
}
