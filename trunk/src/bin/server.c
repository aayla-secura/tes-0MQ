/*
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * ––––––––––––––––––––––––––––––––––– API ––––––––––––––––––––––––––––––––––––
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * 
 *               –––––––                   –––––––
 *               | REQ |                   | SUB |                       client
 *               –––––––                   –––––––
 *                  |                         |
 *
 * ––––––––––– save to file –––––––––––––––– MCA ––––––––––––––––––––––––––––––
 *
 *                  |                         |
 *              –––––––.                   –––––––
 *              | REP |                    | PUB |
 *              –––––––.                   –––––––
 *           
 *              ––––––––                   ––––––––
 *              | PAIR |                   | PAIR |
 *              ––––––––                   ––––––––                     n server
 *                 |                          |
 *              ––––––––– task coordinator ––––––––
 *                 |                          |
 *              ––––––––                   ––––––––
 *              | PAIR |                   | PAIR |           
 *              ––––––––                   ––––––––
 *
 * –––––––––––––––––––––––––––––– REP INTERFACE –––––––––––––––––––––––––––––––
 * 
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
 *       N: no. of ticks written (may be less than requested if error occurred
 *            or if MAX_FSIZE was reached)
 *   Frame 3 (uint64_t):
 *       N: no. of bytes written
 *   Frame 4 (uint64_t):
 *       N: no. of frames written
 *   Frame 5 (uint64_t):
 *       N: no. of dropped frames
 *
 * At the moment we pre-allocate a maximum file size when opening the file and
 * truncate when closing. If the maximum is reached, we simply stop, do not
 * create a new one or enlarge. Should we handle this differently?
 *
 * At the moment we only handle one save-to-file request at a time. Will block
 * until it is done.
 *
 * –––––––––––––––––––––––––––––– PUB INTERFACE –––––––––––––––––––––––––––––––
 *
 * Coming soon
 *
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * –––––––––––––––––––––––––––––––– DEV NOTES –––––––––––––––––––––––––––––––––
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * We use assert throughout to catch bugs. Normally these statements should
 * never be reached regardless of user input. Other errors are handled
 * gracefully with messages to syslog or stderr/out.
 *
 * At the moment we use netmap, since we deal with ethernet ports. To simplify
 * the eventual transition to a PCI interface, we abstract away from netmap by
 * defining wrappers around netmap's methods and types. When transitioning to
 * a PCI interface, a netmap-like API needs to be written and the corresponding
 * wrappers updated.
 *
 * There is a separate thread for each "task". Currently there are two tasks:
 * 1) Listen on a REP socket and save all frames to file (until a requested
 *    number of ticks pass).
 * 2) Collate MCA frames for publishing via a PUB socket.
 *
 * Note: We start the task threads using zactor high-level class, which on UNIX
 * systems is a wrapper around pthread_create. zactor_new creates two PAIR zmq
 * sockets and creates a detached thread caliing a wrapper (s_thread_shim)
 * around the hanlder of our choice. It starts the actual handler (which we
 * pass to zactor_new), passing it its end of the pipe (a PAIR socket) as well
 * as a void* argument of our choice (again, given to zactor_new). The handler
 * must signal down the pipe using zsock_signal (doesn't matter the byte
 * status), since zactor_new will be waiting for this before it returns. The
 * handler must listen on the pipe for a terminating signal, which is sent by
 * the actor's destructor (called by zactor_destroy). Upon receiving this
 * signal the handler must return. It returns into s_thread_shim which signals
 * down the pipe before destroying that end of the pipe. The destructor must
 * wait for this signal before returning into zactor_destroy, which destroys
 * the other end of the pipe and returns to the caller. Hence zactor_destroy
 * acts analogously to pthread_cancel + pthread_join (for joinable threads).
 * The default destructor sends a single-frame message from the string "$TERM".
 * zactor_set_destructor, which can set a custom destructor is a DRAFT method
 * only available in latest commits, so we stick to the default one for now.
 * But since we want to deal with integer signals, and not string messages, we
 * define fpga_destroy as a wrapper around zactor_destroy, which sends SIG_STOP
 * and then calls zactor_destroy to wait for the handler to return.
 *
 * The coordinator thread coordinates between tasks, as described below. There
 * is a separate pipe connection between the coordinator thread and each task
 * thread. See comments for each SIG_* below.
 *
 * Netmap uses two user-driven constructs---a head and a cursor. The head tells
 * it which slots it can safely free, while the cursor tells it when to unblock
 * a poll call. When the head lags behind the tail, the cursor must never be
 * set to a slot index in the range head+1 ... tail because the poll would
 * block forever (the tail will reach the head before it reaches the cursor).
 *
 * The simplest, fastest way to deal with new packets is to move the head and
 * cursor together until they reach the tail (at which point the ring is
 * empty). The next poll will unblock when the ring gets full (the tail will
 * wrap around and reach cursor).
 *
 * To do this in our multi-threaded context each task thread has access to the
 * (read-only) rxring tail, so it knows when it has processed all available
 * packets. It keeps its own head (readable by the coordinator thread), which
 * tells the coordinator thread which packets the task has processed.
 *
 * Task threads do not poll the netmap file descriptor directly, since the poll
 * would return whenever the tail advances past the rxring head, which would
 * lag behind the per-thread head.
 *
 * After receiving new packets, the coordinator thread checks its list of
 * tasks. For all tasks which are currently doing work (indicated by an 'active
 * boolean) it checks their heads and saves the one that is farthest behind the
 * tail (the slowest task). Then, to each task which is waiting for more
 * packets (indicated by a 'busy' boolean being false) it sends a SIG_WAKEUP
 * (down the corresponding pipe). It then sets the rxring cursor and the rxring
 * head to the per-thread head that is farthest behind the tail.
 *
 * Each task thread polls its end of the pipe waiting for a SIG_WAKEUP signal.
 * After receiving a SIG_WAKEUP signal, it sets the 'busy' parameter to true
 * and enters a while loop, processing packets and advancing its own head until
 * it reaches the rxring tail. Then it sets the 'busy' parameter to false.
 *
 * When a task thread is not interested in receiving packets (e.g. when there
 * is no ongoing request to save to file), it sets a 'active' boolean to false.
 * The coordinator thread will not check its head then.
 *
 * Note: bool type and true/false macros are ensured by CZMQ.
 *
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * –––––––––––––––––––––––––––––––––– TO DO –––––––––––––––––––––––––––––––––––
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * fpga.h: fix event length and MAX_PLS_PEAKS
 * check if packet is valid and drop (increment another counter for malformed
 *   packets)
 * how to ensure that task threads do not modigy head or cursor (or any other
 *   netmap variable), and only read them?
 */

#define VERBOSE
#define MULTITHREAD /* this is only used in the debugging macros in common.h */

/* The following macros change parts of the implementation */
// #define BE_DAEMON
// #define CUSTOM_ZACTOR_DESTRUCTOR

#ifdef BE_DAEMON
#  define SYSLOG /* print to syslog */
#  include "daemon.h"
#else /* BE_DAEMON */
#  define UPDATE_INTERVAL 2  /* in seconds */
#endif /* BE_DAEMON */

#include "common.h"

/* Listening interfaces */
#define TASK_SAVE_IF "tcp://*:55555" /* REP */
#define TASK_HIST_IF "tcp://*:55556" /* PUB */
#define FPGA_IF      "vale:fpga}1"

/* Signals for communicating between coordinator and task threads */
#define SIG_INIT   0 /* task -> coordinator thread when ready */
#define SIG_STOP   1 /* coordinator -> task when error or shutting down */
#define SIG_DIED   2 /* task -> coordinator when error */
#define SIG_WAKEUP 3 /* coordinator -> task when new packets arrive */

/* ------------------------------------------------------------------------- */
/* -------------------------- TASK-SPECIFIC DATA --------------------------- */
/* ------------------------------------------------------------------------- */

/* --------------------------- SAVE-TO-FILE TASK --------------------------- */

/* Sent to the client if we don't understand a request */
#define REQ_FAIL        0
#define REQ_OK          1
#define REQ_PIC      "s81"
#define REP_PIC    "18888"

#define FDATA_OFF   40 /* bytes to skip at the beginning of the file, see struct
			* sjob_stats_t */

/*
 * Data related to the currently saved file. Only one thread uses this.
 */
struct sjob_stats_t
{/* statistics saved to the file; we declare it as a separate struct to avoid
  * potential bugs when changing the layout of struct sjob_t or when reading
  * into it from the opened file */
	u_int64_t ticks;
	u_int64_t size;        /* do not exceed MAX_FSIZE */
	u_int64_t frames;
	u_int64_t frames_lost; /* dropped frames */
	/* TO DO: send those flags as well */
	/* Last 8-bytes of the tick header */
	u_int8_t ovrfl;
	u_int8_t err;
	u_int8_t cfd;
	u_int8_t : 8;          /* reserved */
	u_int32_t events_lost;
};
struct sjob_t
{
	struct sjob_stats_t st;
	u_int64_t max_ticks;
	char* filename;
	int fd;
};

/* --------------------------- PUBLISH HIST TASK --------------------------- */

/* ------------------------------------------------------------------------- */
/* -------------------------- DATA FOR ALL TASKS --------------------------- */
/* ------------------------------------------------------------------------- */

/*
 * Per-thread data. Each task thread handler receives this.
 */
struct task_arg_t
{
	struct netmap_ring* rxring; /* we only use one */
	zsock_t*     frontend;   /* the public interface */
	void*        data;       /* task specific, the task sets this */
	u_int32_t    head;       /* private to thread */
	u_int16_t    cur_frame;  /* keep track of missed frames */
	bool         terminated; /* the handler for the PAIR socket to the
				  * coordinator thread will set this when
				  * receiving SIG_STOP, in which case the task
				  * thread won't send SIG_DIED before exiting */
	bool         busy;       /* coordinator won't send SIG_WAKEUP */
	bool         active;     /* coordinator will check the tasks's head */
};

/* ------------------------------------------------------------------------- */
/* ----------------------- DATA FOR THE COORDINATOR ------------------------ */
/* ------------------------------------------------------------------------- */

/*
 * The coordinator should not rely on the layout of struct task_arg_t, so we
 * make this opaque. Coordinator accesses it via the methods below see (TASK
 * MANAGEMENT).
 */
struct __task_t
{
	zactor_t*         actor; /* used as a pipe to task thread */
	struct task_arg_t arg;
};

#ifndef BE_DAEMON
/*
 * Statistics, only used in foreground mode
 */
struct stats_t
{
	struct timeval last_update;
	u_int64_t received;
	u_int64_t missed;
};
#endif /* BE_DAEMON */

struct coordinator_t
{
#ifndef BE_DAEMON
	struct stats_t stats;
#endif /* BE_DAEMON */
	struct netmap_ring* rxring; /* we only use one */
	zlistx_t*    tasks;
};

/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */

static inline fpga_pkt* nm_ring_first_pkt (struct netmap_ring* ring);
static inline fpga_pkt* nm_ring_cur_pkt (struct netmap_ring* ring);
static inline fpga_pkt* nm_ring_pkt (
	struct netmap_ring* ring, uint32_t idx);
static inline fpga_pkt* nm_ring_last_pkt (struct netmap_ring* ring);
static inline u_int16_t nm_ring_first_buf_len (struct netmap_ring* ring);
static inline u_int16_t nm_ring_cur_buf_len (struct netmap_ring* ring);
static inline u_int16_t nm_ring_buf_len (
	struct netmap_ring* ring, uint32_t idx);
static inline u_int16_t nm_ring_last_buf_len (struct netmap_ring* ring);
static uint32_t nm_smaller_buf_id (
	struct netmap_ring* ring, uint32_t ida, uint32_t idb);
static uint32_t nm_larger_buf_id (
	struct netmap_ring* ring, uint32_t ida, uint32_t idb);
static int nm_compare_buf_ids (
	struct netmap_ring* ring, uint32_t ida, uint32_t idb);
typedef struct __task_t task_t;
static int task_new (struct netmap_ring* rxring, zactor_fn* tbody,
	zlistx_t* list, zloop_t* loop);
static void task_stop (zactor_t* self);
static void task_destroy (void** self_p);
static inline bool task_is_active (task_t* self);
static inline bool task_is_waiting (task_t* self);
static inline int task_signal (task_t* self, byte sig);
static inline int task_read (task_t* self, zloop_t* loop);
static inline u_int32_t task_get_head (task_t* self);
static void sjob_finalize (struct task_arg_t* self);
static int  sjob_init (struct sjob_t* sjob, mode_t fmode);
static void sjob_read_stat (struct sjob_t* sjob);
static void sjob_write_stat (struct sjob_t* sjob);
static void sjob_close (struct sjob_t* sjob);
static int coordinator_sig_hn (zloop_t* loop, zsock_t* reader, void* self_);
static int sjob_req_hn (zloop_t* loop, zsock_t* reader, void* self_);
#ifndef BE_DAEMON
static int print_stats (zloop_t* loop, int timer_id, void* stats_);
#endif /* BE_DAEMON */
static int task_sig_hn (zloop_t* loop, zsock_t* reader, void* ignored);
static int new_pkts_hn (zloop_t* loop, zmq_pollitem_t* pitem, void* data_);
static void sjob_task_body (zsock_t* pipe, void* self_);
static void hist_task_body (zsock_t* pipe, void* arg_);
static int coordinator_body (void);

/* ------------------------------------------------------------------------- */
/* ------------------------------- HELPERS --------------------------------- */
/* ------------------------------------------------------------------------- */

/* -------------------------------- NETMAP --------------------------------- */

/* Get the head, cur, <id>, following <id> or tail buffer of a ring. */
static inline fpga_pkt*
nm_ring_first_pkt (struct netmap_ring* ring)
{
	return (fpga_pkt*) NETMAP_BUF (ring, ring->slot[ ring->head ].buf_idx);
}
static inline fpga_pkt*
nm_ring_cur_pkt (struct netmap_ring* ring)
{
	if (unlikely (ring->cur == ring->tail))
		return NULL;
	return (fpga_pkt*) NETMAP_BUF (ring, ring->slot[ ring->cur ].buf_idx);
}
static inline fpga_pkt*
nm_ring_pkt (struct netmap_ring* ring, uint32_t idx)
{
	if (unlikely (idx == ring->tail))
		return NULL;
	return (fpga_pkt*) NETMAP_BUF (ring, ring->slot[ idx ].buf_idx);
}
static inline fpga_pkt*
nm_ring_last_pkt (struct netmap_ring* ring)
{
	int last = ring->tail - 1;
	if (unlikely (last < 0))
		last = ring->num_slots - 1;
	return (fpga_pkt*) NETMAP_BUF (ring, ring->slot[ last ].buf_idx);
}

/* Get length of the head, cur, <id>, following <id> or tail buffer of a ring. */
static inline u_int16_t
nm_ring_first_buf_len (struct netmap_ring* ring)
{
	return ring->slot[ ring->head ].len;
}
static inline u_int16_t
nm_ring_cur_buf_len (struct netmap_ring* ring)
{
	if (unlikely (ring->cur == ring->tail))
		return 0;
	return ring->slot[ ring->cur ].len;
}
static inline u_int16_t
nm_ring_buf_len (struct netmap_ring* ring, uint32_t idx)
{
	if (unlikely (idx == ring->tail))
		return 0;
	return ring->slot[ idx ].len;
}
static inline u_int16_t
nm_ring_last_buf_len (struct netmap_ring* ring)
{
	int last = ring->tail - 1;
	if (unlikely (last < 0))
		last = ring->num_slots - 1;
	return ring->slot[ last ].len;
}

/* Compare slots mod num_slots taking into accout the ring's head. Returns the
 * buf idx that is closer (smaller) or farther (larger) to the ring's head in
 * a forward direction. */
static uint32_t
nm_smaller_buf_id (struct netmap_ring* ring, uint32_t ida, uint32_t idb)
{
	if (unlikely (ida == idb)) /* unlikely defined in netmap_user.h */
		return ida;

	uint32_t min, max;
	if (ida < idb)
	{
		min = ida;
		max = idb;
	}
	else
	{
		min = idb;
		max = ida;
	}
	/* If both are in the same region of the ring (i.e. numerically both
	 * are < or both are > head, then the numerically smaller is first,
	 * otherwise, the numerically larger is first. */
	if ( ring->head <= min || ring->head >= max)
		return min;
	else
		return max;
}
#if 0
/* Not used */
static uint32_t
nm_larger_buf_id (struct netmap_ring* ring, uint32_t ida, uint32_t idb)
{
	if (unlikely (ida == idb))
		return ida;

	uint32_t min, max;
	if (ida < idb)
	{
		min = ida;
		max = idb;
	}
	else
	{
		min = idb;
		max = ida;
	}
	/* If both are in the same region of the ring (i.e. numerically both
	 * are < or both are > head, then the numerically smaller is first,
	 * otherwise, the numerically larger is first. */
	if ( ring->head <= min || ring->head >= max)
		return max;
	else
		return min;
}
/* Returns -1 or 1 if ida is closer or farther from the head than idb. Returns
 * 0 if they are equal. */
static int
nm_compare_buf_ids (struct netmap_ring* ring, uint32_t ida, uint32_t idb)
{
	if (unlikely (ida == idb))
		return 0;

	/* If both are in the same region of the ring (i.e. numerically both
	 * are < or both are > head, then the numerically smaller is first,
	 * otherwise, the numerically larger is first. */
	if ( (ring->head <= ida && ring->head <= idb) ||
		(ring->head >= ida && ring->head >= idb) )
		return (ida < idb) ? -1 : 1;
	else
		return (ida < idb) ? 1 : -1;
}
#endif

/* ---------------------------- TASK MANAGEMENT ---------------------------- */

/*
 * Initializes a struct task_arg_t and starts a new thread using zactor_new.
 * Adds the task to start of list. If loop is not NULL, it registers the task
 * as a generic reader.
 * Returns the task which should be added to either the active or inactive list
 * or NULL if error.
 */
static int
task_new (struct netmap_ring* rxring, zactor_fn* tbody,
	zlistx_t* list, zloop_t* loop)
{
	assert (tbody);
	assert (rxring);

	task_t* self = (task_t*) malloc (sizeof (task_t));
	if (self == NULL)
	{
		assert (errno == ENOMEM);
		ERROR ("malloc failed");
		return -1;
	}
	memset (self, 0, sizeof (*self));
	self->arg.rxring = rxring;
	self->arg.head = rxring->head;

	/* Start the thread, will block until the handler signals */
	self->actor = zactor_new (tbody, &self->arg);
#ifdef CUSTOM_ZACTOR_DESTRUCTOR
	zactor_set_destructor (self->actor, task_stop);
#endif /* CUSTOM_ZACTOR_DESTRUCTOR */
	/* zactor_new does not check the signal, so no way to know if there was
	 * an error. As a workaroung the task thread will send a second signal
	 * when it is ready (or when it fails) and we wait for it here. */
	int rc = zsock_wait (self->actor);
	if (rc == SIG_DIED)
	{
		ERROR ("Task thread failed to initialize");
		task_destroy ((void**)&self);
		return -1;
	}
	assert (rc == SIG_INIT);
	DEBUG ("Task thread initialized");

	if (loop)
	{
		rc = task_read (self, loop);
		if (rc == -1)
		{
			ERROR ("Could not register the zloop readers");
			task_destroy ((void**)&self);
			return -1;
		}
	}
	zlistx_add_start (list, self);
	return 0;
}

#ifdef CUSTOM_ZACTOR_DESTRUCTOR
/*
 * This mirrors the default destructor except that it sends a signal rather
 * than a string message. All of the communication between the coordinator and
 * task thread is using ZMQ signals. 
 */
static void
task_stop (zactor_t* self)
{
	assert (self);
	if (zsock_signal (self, SIG_STOP) == 0)
	{
		zsock_wait (self); /* zactor_new starts a wrapper around the
				    * requested handler which signals when the
				    * task handler returns, so this is
				    * analogous to pthread_join */
	}
}
#endif /* CUSTOM_ZACTOR_DESTRUCTOR */

/*
 * This is to be used instead of zactor_destroy, partly as a workaround in case
 * of not setting a custom destructor. Task needs to be removed from list first.
 */
static void
task_destroy (void** self_p)
{
	assert (self_p);
#ifndef CUSTOM_ZACTOR_DESTRUCTOR
	task_t* self = *self_p;
	if (self)
	{
		zsock_signal (self->actor, SIG_STOP);
#endif /* CUSTOM_ZACTOR_DESTRUCTOR */
		/* If not using task_stop as a custom destructor,
		 * zactor_destroy will send "$TERM" which will be ignored; not
		 * a problem. */
		zactor_destroy (&self->actor);
		free (self);
	}
	*self_p = NULL;
}

/*
 * Main will only consider ring heads for active tasks
 */
static inline bool
task_is_active (task_t* self)
{
	assert (self);
	return self->arg.active;

}

/*
 * Main will only send SIG_WAKEUP to waiting threads
 */
static inline bool
task_is_busy (task_t* self)
{
	assert (self);
	return self->arg.busy;

}

/*
 * Send a signal to task
 */
static inline int
task_signal (task_t* self, byte sig)
{
	assert (self);
	return zsock_signal (self->actor, sig);
}

/*
 * Register a coordinator's PAIR socket to a task as a zloop reader.
 * It handles SIG_DIED. SIG_INIT is handled by task_new.
 */
static inline int
task_read (task_t* self, zloop_t* loop)
{
	assert (self);
	return zloop_reader (loop, zactor_sock(self->actor),
		task_sig_hn, NULL);
}

/*
 * Get the head of the slowest task.
 */
static inline u_int32_t
task_get_head (task_t* self)
{
	assert (self);
	return self->arg.head;
}

/* --------------------------- SAVE-TO-FILE TASK --------------------------- */

/*
 * Called by the main thread when a save job is done or when we are shutting
 * down. Will send a message to the client.
 */
static void
sjob_finalize (struct task_arg_t* self)
{
	assert (self);
	/* TO DO: check if this should only be called when active */
	if ( ! self->active )
	{
		return;
	}
	struct sjob_t* sjob = (struct sjob_t*) self->data;
	assert (sjob->filename != NULL);
	assert (sjob->fd != -1);
	self->active = false;

	/* Send message to client */
	zsock_send (self->frontend, REP_PIC, REQ_OK, 
		sjob->st.ticks,
		sjob->st.size,
		sjob->st.frames,
		sjob->st.frames_lost);

	sjob_write_stat (sjob);
	sjob_close (sjob);
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
	assert (sjob->filename != NULL);
	assert (sjob->fd == -1);
	assert (sjob->st.ticks == 0);
	assert (sjob->st.size == 0);
	assert (sjob->st.frames == 0);
	assert (sjob->st.frames_lost == 0);
	assert (sjob->st.ovrfl == 0);
	assert (sjob->st.err == 0);
	assert (sjob->st.cfd == 0);
	assert (sjob->st.events_lost == 0);

	/* Open the file */
	sjob->fd = open (sjob->filename, fmode,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (sjob->fd == -1)
	{ /* do not print warning, maybe request was for status */
		return -1;
	}

	int rc;
	rc = lseek (sjob->fd, FDATA_OFF, 0);
	if (rc == (off_t)-1 || rc < FDATA_OFF)
	{
		WARN ("Could not adjust file cursor");
		return -1;
	}

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
	assert (sjob->filename != NULL);
	assert (sjob->fd != -1);
	assert (sjob->st.ticks == 0);
	assert (sjob->st.size == 0);
	assert (sjob->st.frames == 0);
	assert (sjob->st.frames_lost == 0);
	assert (sjob->st.ovrfl == 0);
	assert (sjob->st.err == 0);
	assert (sjob->st.cfd == 0);
	assert (sjob->st.events_lost == 0);

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
}

/*
 * Writes stats to a currently open file. Used right before closing it.
 */
static void
sjob_write_stat (struct sjob_t* sjob)
{
	assert (sjob);
	assert (sjob->filename != NULL);
	assert (sjob->fd != -1);

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
}

/*
 * Unmaps mmapped file, closes the file descriptor, nullifies and resets stats
 */
static void
sjob_close (struct sjob_t* sjob)
{
	assert (sjob);

	if (sjob->fd >= 0)
	{
		close (sjob->fd);
		sjob->fd = -1;
	}

	zstr_free (&sjob->filename);
	sjob->max_ticks = 0;
	sjob->st.ticks = 0;
	sjob->st.size = 0;
	sjob->st.frames = 0;
	sjob->st.frames_lost = 0;
	sjob->st.ovrfl = 0;
	sjob->st.err = 0;
	sjob->st.cfd = 0;
	sjob->st.events_lost = 0;
}

/* --------------------------- PUBLISH HIST TASK --------------------------- */

/* ------------------------------------------------------------------------- */
/* --------------------------- SOCKET HANDLERS ----------------------------- */
/* ------------------------------------------------------------------------- */

/*
 * Called when the coordinator thread signals a task thread after.
 * At SIG_STOP we exit, at SIG_WAKEUP we process packets in the ring.
 */
static int
coordinator_sig_hn (zloop_t* loop, zsock_t* reader, void* self_)
{
	assert (self_);

	struct task_arg_t* self = (struct task_arg_t*) self_;
	assert ( ! self->terminated );
	assert ( ! self->busy );
	
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
	if (sig == SIG_STOP)
	{
		DEBUG ("Coordinator thread is terminating us");
		self->terminated = true;
		return -1;
	}
	assert (sig == SIG_WAKEUP);
	assert (self->active);

	struct sjob_t* sjob = (struct sjob_t*) self->data;
	assert (sjob->filename != NULL);
	assert (sjob->fd != -1);
	self->busy = true;
	/* Process packets */
	while (true)
	{
		fpga_pkt* pkt = nm_ring_pkt (self->rxring, self->head);
		if (pkt == NULL)
			break;

		uint16_t len = nm_ring_buf_len (self->rxring, self->head);
		if (len != pkt_len (pkt))
		{
			WARN ("Packet length mismatch: header says %hu, "
				"netmap slot is %hu", pkt_len (pkt), len);
			if (len < pkt_len (pkt))
				len = pkt_len (pkt);
		}
		int rc = __check_fpga_pkt (pkt);
		if (rc)
		{ /* TO DO */
			;
		}

		/* TO DO: how to handle errors */
		rc = write (sjob->fd, pkt, len);
		if (rc == -1)
		{
			ERROR ("Could not write to file [%d]", sjob->fd);
			return -1;
		}

		uint16_t prev_frame = self->cur_frame;
		self->cur_frame = frame_seq (pkt);

		/* TO DO: save err flags */
		if (is_tick (pkt))
			sjob->st.ticks++;
		sjob->st.size += len;
		sjob->st.frames++;
		sjob->st.frames_lost += (u_int64_t) (
			(uint16_t)(self->cur_frame - prev_frame) - 1);

		if (sjob->st.ticks == sjob->max_ticks)
		{
			sjob_finalize (self);
			assert ( ! self->active );
			/* Enable polling on the reader */
			rc = zloop_reader (loop, self->frontend,
				sjob_req_hn, self_);
			if (rc == -1)
			{
				ERROR ("Could not re-enable the zloop reader");
				return -1;
			}
			break;
		}

		self->head = nm_ring_next (self->rxring, self->head);
	}

	self->busy = false;
	return 0;
}

/* --------------------------- SAVE-TO-FILE TASK --------------------------- */

/*
 * Called when a client sends a request on the REP socket. For valid requests
 * of status, opens the file and send the reply. For valid requests to save,
 * opens the file and marks the task as active, so that next time the
 * coordinator reads new packets it will send a SIG_WAKEUP.
 */
static int
sjob_req_hn (zloop_t* loop, zsock_t* reader, void* self_)
{
	assert (self_);

	struct task_arg_t* self = (struct task_arg_t*) self_;
	assert ( ! self->terminated );
	assert ( ! self->busy );
	assert ( ! self->active );
	struct sjob_t* sjob = (struct sjob_t*) self->data;
	u_int8_t job_mode;

	int rc = zsock_recv (reader, REQ_PIC, &sjob->filename,
		&sjob->max_ticks, &job_mode);
	if (rc == -1)
	{ /* would also return -1 if picture contained a pointer (p) or a null
	   * frame (z) but message received did not match this signature; this
	   * is irrelevant in this case */
		DEBUG ("Receive interrupted");
		return -1;
	}
	if (sjob->filename == NULL || job_mode > 1)
	{
		INFO ("Received a malformed request");
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
	if (sjob->max_ticks == 0)
	{ /* status */
		INFO ("Received request for status");
		fmode = O_RDONLY;
		exp_errno = ENOENT;
	}
	else
	{
		INFO ("Received request to write %lu ticks", sjob->max_ticks);
		fmode = O_RDWR | O_CREAT;
		if (job_mode == 0)
		{ /* do not overwrite */
				fmode |= O_EXCL;
				exp_errno = EEXIST;
		}
	}

	rc = sjob_init (sjob, fmode);
	if (rc == -1)
	{
		if (errno != exp_errno)
		{
			WARN ("Could not open file %s",
				sjob->filename);
		}
		zsock_send (reader, REP_PIC, REQ_FAIL, 0, 0, 0, 0);
		sjob_close (sjob);
		return 0;
	}
	INFO ("Opened file %s for %s", sjob->filename,
		(fmode & O_RDWR ) ? "writing" : "reading" );

	if (sjob->max_ticks == 0)
	{ /* just read in stats and send reply */
		sjob_read_stat (sjob);
		zsock_send (reader, REP_PIC, REQ_OK, 
			sjob->st.ticks,
			sjob->st.size,
			sjob->st.frames,
			sjob->st.frames_lost);
		sjob_close (sjob);
		return 0;
	}

	/* Disable polling on the reader until the job is done */
	zloop_reader_end (loop, reader);
	self->head = self->rxring->head;
	self->active = true;
	return 0;
}

/* --------------------------- PUBLISH HIST TASK --------------------------- */

/* ------------------------------ COORDINATOR ------------------------------ */

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
		"dropped frames: %10lu    "
		"avg bandwidth: %10.3e pps",
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
 * Called when a task thread signals the coordinator thread after polling has
 * started. At the moment we only deal with SIG_DIED upon error.
 */
static int
task_sig_hn (zloop_t* loop, zsock_t* reader, void* ignored)
{
	DEBUG ("Got a signal from task");

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
		case SIG_DIED:
			DEBUG ("Task thread encountered an error");
			return -1;
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
 * Called when new packets arrive in the ring.
 */
static int
new_pkts_hn (zloop_t* loop, zmq_pollitem_t* pitem, void* data_)
{
	assert (data_);
	struct coordinator_t* data = (struct coordinator_t*) data_;

	/* Signal the waiting tasks and find the head of the slowest one */
	u_int32_t head = data->rxring->tail; /* in case no active tasks */
	task_t* task = zlistx_first (data->tasks);
	int rc = 0;
	while (task)
	{
		if (task_is_active (task))
		{
			head = nm_smaller_buf_id (data->rxring,
				head, task_get_head (task));
			if ( ! task_is_busy (task) )
				rc |= task_signal (task, SIG_WAKEUP);
		}
		task = zlistx_next (data->tasks);
	}
	if (rc)
	{
		ERROR ("Could not send SIG_WAKEUP to all waiting tasks.");
		return -1;
	}

	/* Save statistics */
	uint32_t num_new = nm_ring_space (data->rxring);
	data->stats.received += num_new;
	fpga_pkt* pkt = nm_ring_cur_pkt (data->rxring);
	uint16_t fseqA = frame_seq (pkt);
	pkt = nm_ring_last_pkt (data->rxring);
	uint16_t fseqB = frame_seq (pkt);
	data->stats.missed += (u_int64_t) ( num_new - 1 -
		(uint16_t)(fseqB - fseqA) );

	/* Set the head and cursor */
	data->rxring->head = data->rxring->cur = head;

	return 0;
}

/* ------------------------------------------------------------------------- */
/* --------------------------- THREADS' BODIES ----------------------------- */
/* ------------------------------------------------------------------------- */

/* See comments in DEV NOTES section at the beginning */

static void
sjob_task_body (zsock_t* pipe, void* self_)
{
	assert (self_);
	zsock_signal (pipe, 0); /* zactor_new will wait for this */

	int rc;
	struct task_arg_t* self = (struct task_arg_t*) self_;
	assert ( ! self->terminated );
	assert ( ! self->busy );
	assert ( ! self->active );

	struct sjob_t sjob;
	memset (&sjob, 0, sizeof (sjob));
	sjob.fd = -1;
	self->data = &sjob;

	zloop_t* loop = zloop_new ();
	zloop_set_nonstop (loop, 1); /* only the coordinator thread should get
				      * interrupted, we wait for SIG_STOP */

	/* Open the REP interface */
	self->frontend = zsock_new_rep ("@"TASK_SAVE_IF);
	if (self->frontend == NULL)
	{
		ERROR ("Could not open the public socket");
		goto cleanup;
	}
	INFO ("Opened the REP interface");

	/* Register the readers */
	rc  = zloop_reader (loop, pipe, coordinator_sig_hn, self_);
	rc |= zloop_reader (loop, self->frontend, sjob_req_hn, self_);
	if (rc)
	{
		ERROR ("Could not register the zloop readers");
		goto cleanup;
	}

	zsock_signal (pipe, SIG_INIT); /* task_new will wait for this */
	DEBUG ("Waiting for requests");
	rc = zloop_start (loop);
	assert (rc == -1); /* we don't get interrupted */

cleanup:
	sjob_finalize (self); /* will set active to false */
	assert ( ! self->active );
	/*
	 * zactor_destroy waits for a signal from s_thread_shim (see DEV
	 * NOTES), so if we exited the loop after receiving SIG_STOP (from
	 * task_destroy, which calls zactor_destroy), we don't signal SIG_DIED.
	 * We only signal the coordinator thread here if we exited due to an
	 * error on our part (in the REP handler). In that case, the
	 * coordinator thread will exit its loop and call task_destroy to
	 * collect the final signal from s_thread_shim.
	 */
	if ( ! self->terminated )
		zsock_signal (pipe, SIG_DIED);
	zloop_destroy (&loop);
	zsock_destroy (&self->frontend);
	DEBUG ("Done");
}

static void
hist_task_body (zsock_t* pipe, void* arg_)
{
}

static int
coordinator_body (void)
{
	int rc;
	struct coordinator_t data;
	memset (&data, 0, sizeof (data));

	/* Open the interface */
	struct nm_desc* nmd = nm_open(FPGA_IF, NULL, 0, 0);
	if (nmd == NULL)
	{
		ERROR ("Could not open interface %s", FPGA_IF);
		return -1;
	}
	INFO ("Opened interface %s", FPGA_IF);

	/* Get the ring (we only use one) */
	assert (nmd->last_rx_ring == nmd->first_rx_ring);
	data.rxring = NETMAP_RXRING (nmd->nifp,
		nmd->first_rx_ring);
	assert (data.rxring);

	/* Start the tasks and register the readers. Tasks are initialized as
	 * inactive. */
	data.tasks = zlistx_new ();
	zlistx_set_destructor (data.tasks, task_destroy);
	zloop_t* loop = zloop_new ();

	/* REP handler */
	rc  = task_new (data.rxring, sjob_task_body, data.tasks, loop);
	/* PUB handler */
	/* rc |= task_new (data.rxring, hist_task_body, data.tasks, loop); */
	if (rc)
	{
		goto cleanup;
	}

	/* Register the FPGA interface as a poller */
	struct zmq_pollitem_t pitem;
	memset (&pitem, 0, sizeof (pitem));
	pitem.fd = nmd->fd;
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
	zlistx_destroy (&data.tasks);
	zloop_destroy (&loop);
	nm_close (nmd);
	DEBUG ("Done");
	return rc;
}

/* ------------------------------------------------------------------------- */
/* --------------------------------- MAIN ---------------------------------- */
/* ------------------------------------------------------------------------- */

int
main (void)
{
	assert (sizeof (struct sjob_stats_t) == FDATA_OFF);
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