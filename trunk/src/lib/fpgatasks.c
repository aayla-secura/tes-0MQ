/*
 * ————————————————————————————————————————————————————————————————————————————
 * ——————————————————————————————————— API ————————————————————————————————————
 * ————————————————————————————————————————————————————————————————————————————
 * 
 *               ———————                   ———————
 *               | REQ |                   | SUB |                       client
 *               ———————                   ———————
 *                  |                         |
 *
 * ——————————— save to file ————————————— histogram ———————————————————————————
 *
 *                  |                         |
 *              ———————.                   ———————
 *              | REP |                    | PUB |
 *              ———————.                   ———————
 *           
 *              ————————                   ————————
 *              | PAIR |                   | PAIR |
 *              ————————                   ————————                      server
 *                 |                          |
 *              ————————— task coordinator ————————
 *                 |                          |
 *              ————————                   ————————
 *              | PAIR |                   | PAIR |           
 *              ————————                   ————————
 *
 * —————————————————————————————— REP INTERFACE ———————————————————————————————
 * 
 * Messages are sent and read via zsock_send, zsock_recv as "picture" messages.
 * Valid requests have a picture of "s81", replies have a picture of "18888":
 * 
 * Format for valid save requests is:
 *   Frame 1 (char*):
 *       Filename. It is relative to a hardcoded root, leading slashes are
 *       ignored. Trailing slashes are not allowed. Missing directories are
 *       created.
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
 * At the moment we only handle one save-to-file request at a time. Will block
 * until it is done.
 *
 * —————————————————————————————— PUB INTERFACE ———————————————————————————————
 * Sends ZMQ single-frame messages (they are long, so will be fragmented on the
 * wire), each message contains one full histogram. You can receive these with
 * zmq_recv for example.
 *
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * –––––––––––––––––––––––––––––––– DEV NOTES –––––––––––––––––––––––––––––––––
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * There is a separate thread for each "task". Threads are zactors.
 * Currently there are two tasks:
 * 1) Listen on a REP socket and save all frames to file (until a requested
 *    number of ticks pass).
 * 2) Collate MCA frames for publishing via a PUB socket.
 *
 * Tasks have read-only access to rings (they cannot modify the cursor or head)
 * and each task keeps its own head (for each ring), which is visible by the
 * coordinator (fpgacoord.c). For each ring, the coordinator sets the true head
 * to the per-task head which lags behind all others.
 *
 * Tasks are largely similar, so we pass the same handler, s_task_shim, to
 * zactor_new. It is responsible for doing most of the work.
 * Tasks are described by the following structure:
 * struct _task_t
 * {
 *         zloop_reader_fn* client_handler;
 *         s_pkt_fn*        pkt_handler;
 *         s_data_fn*       data_init; // initialize data
 *         s_data_fn*       data_fin;  // cleanup data
 *         void*       data;           // task-specific
 *         zactor_t*   shim;           // coordinator's end of the pipe, signals
 *                                     // sent on behalf of coordinator go here
 *         zsock_t*    frontend;       // clients
 *         const char* front_addr;     // the socket addresses, comma separated
 *         const int   front_type;     // one of ZMQ_*
 *         int         id;             // the task ID
 *         ifdesc*     ifd;            // netmap interface
 *         uint32_t    heads[NUM_RINGS]; // per-ring task's head
 *         uint16_t    nrings;         // number of rings <= NUM_RINGS
 *         uint16_t    prev_fseq;      // previous frame sequence
 *         uint16_t    prev_pseq_mca;  // previous MCA protocol sequence
 *         uint16_t    prev_pseq_tr;   // previous trace protocol sequence
 *         uint16_t    prev_pseq_pls;  // previous pulse protocol sequence
 *         bool        error;          // client_ and pkt_handler should set this
 *         bool        busy;
 *         bool        active;         // client_handler or data_init should
 *                                     // enable this
 *         bool        autoactivate;   // s_task_shim will activate task
 * };
 *
 * s_task_shim registers a generic reader, s_sig_hn, for handling the signals
 * from the coordinator. Upon SIG_STOP s_sig_hn exits, upon SIG_WAKEUP it calls
 * calls the task's specific packet handler for each packet in each ring. 
 * It keeps track of the previous frame and protocol sequences (the task's
 * packet handler can make use of those as well, e.g. to track lost frames).
 * s_sig_hn also takes care of updating the task's head.
 *
 * If the task defines a public interface address, s_task_shim will open the
 * socket, and if it defines a client handler, it will register it with the
 * task's loop. Each task has a pointer for its own data.
 *
 * Before entering the loop, s_task_shim will call the task initializer, if it
 * is set. So it can allocate the pointer to its data and do anything else it
 * wishes (talk to clients, etc).
 *
 * Right after the loop terminates, s_task_shim will call the task finalizer,
 * so it can cleanup its data and possibly send final messages to clients.
 *
 * The actual task is done inside client_handler and pkt_handler.
 *
 *   client_handler processes messages on the public socket. If front_addr is
 *   not set, the task has no public interface.
 *
 *   pkt_handler is called by the generic socket reader for each packet in each
 *   ring and does whatever.
 *
 * Both handlers have access to the zloop so they can enable or disable readers
 * (e.g. the client_handler can disable itself after receiving a job and the
 * pkt_handler can re-enable it when done).
 * If either handler encounters an error, it sets the task's error flag to true
 * and returns with -1.
 * pkt_handler may return with -1 without setting error, if it wants to wait
 * for the next WAKEUP. s_sig_hn will only deactivate the task if error is set,
 * the pkt_handler should set active to false if it won't be processing packets
 * for some time.
 *
 * If the task is not interested in receiving packets, is sets its active flag
 * to false. It won't receive SIG_WAKEUP if it is not active and its heads
 * won't be synchronized with the real heads. When it needs to process packets,
 * it must set its private heads to the global heads (by calling ifring_head
 * for each ring) and then set its active flag to true.
 * Tasks are initialized as inactive, the task should enable the flag either in
 * its initializer or in its client frontend handler.
 *
 * Tasks are defined in a static global array, see THE FULL LIST.
 *
 * Note on zactor:
 * We start the task threads using zactor high-level class, which on UNIX
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
 * define s_task_stop as a wrapper around zactor_destroy, which sends SIG_STOP
 * and then calls zactor_destroy to wait for the handler to return.
 *
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * –––––––––––––––––––––––––––––––––– TO DO –––––––––––––––––––––––––––––––––––
 * ––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––––
 * - Print debugging stats every UPDATE_INTERVAL via the coordinator.
 * - Check if packet is valid and drop (increment another counter for malformed
 *   packets).
 * - Check if repeating the loop over all rings until no more packets is better
 *   than exiting and waiting for a WAKEUP.
 * - Set umask for the save-to-file task.
 * - Check filename for non-printable and non-ASCII characters.
 * - Why does writing to file fail with "unhandled syscall" when running under
 *   valgrind? A: https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=219715
 */

#include "fpgatasks.h"
#include "net/fpgaif_reader.h"
#include "common.h"
#include "aio.h"

/* From netmap_user.h */
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define NUM_RINGS 4 /* number of rx rings in interface */

/* ---------------------------------- API ---------------------------------- */

typedef int (s_data_fn)(task_t*);
typedef int (s_pkt_fn)(zloop_t*, fpga_pkt*, uint16_t, uint16_t, task_t*);

/* See DEV NOTES */
struct _task_t
{
	zloop_reader_fn* client_handler;
	s_pkt_fn*        pkt_handler;
	s_data_fn*       data_init;
	s_data_fn*       data_fin;
	void*       data;
	zactor_t*   shim;
	zsock_t*    frontend;
	const char* front_addr;
	const int   front_type;
	int         id;
	ifdesc*     ifd;
	uint32_t    heads[NUM_RINGS];
	uint16_t    nrings;
	uint16_t    prev_fseq;
	uint16_t    prev_pseq_mca;
	uint16_t    prev_pseq_tr;
	uint16_t    prev_pseq_pls;
	bool        error;
	bool        busy;
	bool        active;
	bool        autoactivate;
#ifdef FULL_DBG
	struct
	{
		uint64_t wakeups;
		uint64_t wakeups_inactive; /* woken up when inactive */
		uint64_t wakeups_false; /* woken up when no new packets */
		uint64_t rings_dispatched;
		struct
		{
			uint64_t rcvd;
			uint64_t missed;
		} pkts;
	} dbg_stats;
#endif
};

/* -------------------------------- HELPERS -------------------------------- */

/* Signals for communicating between coordinator and task threads */
#define SIG_INIT   0 /* task -> coordinator thread when ready */
#define SIG_STOP   1 /* coordinator -> task when error or shutting down */
#define SIG_DIED   2 /* task -> coordinator when error */
#define SIG_WAKEUP 3 /* coordinator -> task when new packets arrive */

static zloop_reader_fn s_sig_hn;
static zloop_reader_fn s_die_hn;
static zactor_fn       s_task_shim;

static int  s_task_start (ifdesc* ifd, task_t* self);
static void s_task_stop (task_t* self);
static inline void s_task_activate (task_t* self);
static int s_task_dispatch (task_t* self, zloop_t* loop,
		uint16_t ring_id, uint16_t missed);

/* --------------------------- SAVE-TO-FILE TASK --------------------------- */

/* See api.h */
#define REQ_FAIL        0
#define REQ_OK          1
#define REQ_PIC      "s81"
#define REP_PIC    "18888"

#ifndef PATH_MAX
#  ifdef MAXPATHLEN
#    define PATH_MAX MAXPATHLEN
#  else
#    define PATH_MAX 4096
#  endif
#endif

#define TSAVE_ROOT "/media/" // must have a trailing slash
#define TSAVE_ONLYFILES      // for now we don't generate filenames
#define TSAVE_SOFFSET  40 // beginning of file reserved for statistics
/* Employ a buffer zone for asynchronous writing. We memcpy frames into the
 * bufzone, between its head and cursor (see s_task_save_data_t below) and
 * queue batches with aio_write. */
#define TSAVE_BUFSIZE  15728640UL // 15 MB 

/*
 * Statistics sent as a reply and saved to the file. 
 */
struct s_task_save_stats_t
{
	uint64_t ticks;
	uint64_t size;           // number of written bytes 
	uint64_t frames;         // total frames saved
	uint64_t frames_lost;    // total frames lost (includes dropped)
	uint64_t errors;         // TO DO: last 8-bytes of the tick header 
};

/*
 * Data for the currently-saved file. max_ticks and filename are set when
 * receiving a request from client.
 */
struct s_task_save_data_t
{
	struct aiocb aios;
	struct s_task_save_stats_t st;
	struct
	{
		size_t waiting;      // copied into buffer since the last aio_write
		size_t enqueued;     // queued for writing at the last aio_write
		unsigned char* base; // mmapped, size of TSAVE_BUFSIZE
		unsigned char* tail; // start address queued for aio_write
		unsigned char* cur;  // address where next packet will be coppied to
		unsigned char* ceil; // base + TSAVE_BUFSIZE
	} bufzone;
	uint64_t max_ticks;
#ifdef FULL_DBG
	size_t prev_enqueued;
	size_t prev_waiting;
	size_t last_written;
	uint64_t batches;
	uint64_t failed_batches;
	uint64_t num_cleared;
	unsigned char prev_hdr[FPGA_HDR_LEN];
#endif
	char*    filename;
};

static zloop_reader_fn s_task_save_req_hn;
static s_pkt_fn        s_task_save_pkt_hn;
static s_data_fn       s_task_save_init;
static s_data_fn       s_task_save_fin;

static int   s_task_save_open  (struct s_task_save_data_t* sjob, mode_t fmode);
static int   s_task_save_queue (struct s_task_save_data_t* sjob, bool force);
static int   s_task_save_read  (struct s_task_save_data_t* sjob);
static int   s_task_save_write (struct s_task_save_data_t* sjob);
static int   s_task_save_send  (struct s_task_save_data_t* sjob,
	zsock_t* frontend);
static void  s_task_save_close (struct s_task_save_data_t* sjob);
static char* s_task_save_canonicalize_path (const char* filename,
	int checkonly, int task_id);

/* --------------------------- PUBLISH HIST TASK --------------------------- */

#if 0  /* FIX */
#define THIST_MAXSIZE 65528U // highest 16-bit number that is a multiple of 8 bytes
#else
#define THIST_MAXSIZE 65576U // highest 16-bit number that is a multiple of 8 bytes
#endif

/*
 * Data for currently built histogram.
 */
struct s_task_hist_data_t
{
#ifdef FULL_DBG
	uint64_t      published; // number of published histograms
	uint64_t      dropped;   // number of aborted histograms
#endif
	uint16_t      nbins;     // total number of bins in histogram
	uint16_t      size;      // size of histogram including header
	uint16_t      cur_nbins; // number of received bins so far
#if 0 /* FIX */
	uint16_t      cur_size;  // number of received bytes so far
#else
	uint32_t      cur_size;  // number of received bytes so far
#endif
	bool          discard;   // discard all frames until the next header
	unsigned char buf[THIST_MAXSIZE];
};

/* There is no handler for the PUB socket. */
static s_pkt_fn        s_task_hist_pkt_hn;
static s_data_fn       s_task_hist_init;
static s_data_fn       s_task_hist_fin;

/* ----------------------------- THE FULL LIST ----------------------------- */

#define NUM_TASKS 2
static task_t tasks[] = {
	{ // SAVE TO FILE 
		.client_handler = s_task_save_req_hn,
		.pkt_handler    = s_task_save_pkt_hn,
		.data_init      = s_task_save_init,
		.data_fin       = s_task_save_fin,
		.front_addr     = "tcp://*:55555",
		.front_type     = ZMQ_REP,
	},
	{ // PUBLISH HIST
		.pkt_handler    = s_task_hist_pkt_hn,
		.data_init      = s_task_hist_init,
		.data_fin       = s_task_hist_fin,
		.front_addr     = "tcp://*:55556",
		.front_type     = ZMQ_PUB,
		.autoactivate   = 1,
	}
};

/* ------------------------------------------------------------------------- */
/* ---------------------------------- API ---------------------------------- */
/* ------------------------------------------------------------------------- */

/*
 * Start the tasks and if c_loop is not NULL, register a generic reader for
 * each task.
 * Returns 0 on success, -1 on error.
 */
int
tasks_start (ifdesc* ifd, zloop_t* c_loop)
{
	dbg_assert (ifd != NULL);
	dbg_assert (NUM_TASKS == sizeof (tasks) / sizeof (task_t));
	int rc;
	for (int t = 0; t < NUM_TASKS; t++)
	{
		tasks[t].id = t + 1;
		s_msgf (0, LOG_DEBUG, 0, "Starting task #%d", t);
		rc = s_task_start (ifd, &tasks[t]);
		if (rc)
		{
			s_msg (errno, LOG_ERR, 0,
				"Could not start tasks");
			return -1;
		}
	}

	if (c_loop != NULL)
		return tasks_read (c_loop);
	return 0;
}

/*
 * Register a generic reader with the loop. The reader will listen to all tasks
 * and terminate the loop when a task dies. This is called by tasks_start if
 * a non-NULL zloop_t* is passed.
 */
int
tasks_read (zloop_t* loop)
{
	dbg_assert (loop != NULL);
	int rc;
	for (int t = 0; t < NUM_TASKS; t++)
	{
		s_msgf (0, LOG_DEBUG, 0, "Registering reader for task #%d", t);
		task_t* self = &tasks[t];
		rc = zloop_reader (loop, zactor_sock(self->shim),
			s_die_hn, NULL);
		if (rc)
		{
			s_msg (errno, LOG_ERR, 0,
				"Could not register the zloop readers");
			return -1;
		}
	}
	return 0;
}

/*
 * Deregister the reader of each task with the loop.
 */
void
tasks_mute (zloop_t* loop)
{
	dbg_assert (loop != NULL);
	for (int t = 0; t < NUM_TASKS; t++)
	{
		s_msgf (0, LOG_DEBUG, 0,
			"Unregistering reader for task #%d", t);
		task_t* self = &tasks[t];
		zloop_reader_end (loop, zactor_sock(self->shim));
	}
}

/*
 * Sends a wake up signal to all tasks waiting for more packets.
 */
int
tasks_wakeup (void)
{
	for (int t = 0; t < NUM_TASKS; t++)
	{
		task_t* self = &tasks[t];
		if (self->active && ! self->busy)
		{
			int rc = zsock_signal (self->shim, SIG_WAKEUP);
			if (rc)
			{
				s_msgf (errno, LOG_ERR, 0,
					"Could not signal task #%d", t);
				return -1;
			}
		}
	}
	return 0;
}

/*
 * Asks each task to terminate and cleans up.
 */
void
tasks_destroy (void)
{
	for (int t = 0; t < NUM_TASKS; t++)
	{
		s_msgf (0, LOG_DEBUG, 0, "Stopping task #%d", t);
		task_t* self = &tasks[t];
		s_task_stop (self);
	}
}

/*
 * For each ring, returns the head of the slowest active task.
 * If no active tasks, returns NULL.
 */
uint32_t*
tasks_get_heads (void)
{
	/* Use a static storage for the returned array. */
	static uint32_t heads[NUM_RINGS];

	int updated = 0; /* set to 1 if at least one active task */
	for (int t = 0; t < NUM_TASKS; t++)
	{
		task_t* self = &tasks[t];
		if (self->active)
		{
			/* The first time an active task is found, take its
			 * head, for each following active task, compare its
			 * head with the currently slowest one. */
			if (updated)
			{
				for (int r = 0; r < NUM_RINGS; r++)
				{
					ifring* rxring = if_rxring (self->ifd, r);
					heads[r] = ifring_earlier_id (
							rxring, heads[r],
							self->heads[r]);
				}
			}
			else
			{
				for (int r = 0; r < NUM_RINGS; r++)
					heads[r] = self->heads[r];
				updated = 1;
			}
		}
	}
	return (updated ? heads : NULL);
}


/* ------------------------------------------------------------------------- */
/* -------------------------------- HELPERS -------------------------------- */
/* ------------------------------------------------------------------------- */

/*
 * Registered with each task's loop. Receives signals sent on behalf of the
 * coordinator (via tasks_wakeup or tasks_stop). On SIG_WAKEUP calls the task's
 * packet handler. On SIG_STOP terminates the task's loop. 
 */
static int
s_sig_hn (zloop_t* loop, zsock_t* reader, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;
	dbg_assert ( ! self->busy );
	
#ifdef FULL_DBG
	/* Catch bugs by receiving a message and asserting it's a signal.
	 * zsock_wait discards messages until a signal arrives. */
	zmsg_t* msg = zmsg_recv (reader);
	if (msg == NULL)
	{
		s_msg (0, LOG_DEBUG, self->id, "Receive interrupted");
		return -1;
	}
	int sig = zmsg_signal (msg);
	zmsg_destroy (&msg);
	dbg_assert (sig >= 0);
#else
	int sig = zsock_wait (reader);
	if (sig == -1)
	{
		s_msg (0, LOG_DEBUG, self->id, "Receive interrupted");
		return -1;
	}
#endif
	if (sig == SIG_STOP)
	{
		s_msg (0, LOG_DEBUG, self->id,
			"Coordinator thread is terminating us");
		return -1;
	}
	dbg_assert (sig == SIG_WAKEUP);
	dbg_assert ( ! self->busy );

	/* FIX: We should never have received a WAKEUP if we are not active,
	 * but I saw this once after encountering write errors while running
	 * under valgrind. */
	if ( ! self->active )
	{
#ifdef FULL_DBG
		self->dbg_stats.wakeups_inactive++;
#endif
		return 0;
	}
#ifdef FULL_DBG
	self->dbg_stats.wakeups++;
#endif
	self->busy = 1;
	/*
	 * Process packets. Find the ring that contains the next packet in
	 * sequence. Allowing for lost frames, simply take the ring for which
	 * the task's head packet is closest in sequence to the last seen frame
	 * sequence.
	 */
	/* TO DO: check all rings */
	uint16_t missed = ~0;  /* will hold minimum jump in frame seq,
			        * initialize to UINT16_MAX */
	int next_ring_id = -1; /* next to process */
	for (int r = 0; r < NUM_RINGS; r++)
	{
		ifring* rxring = if_rxring (self->ifd, r);
		if (ifring_tail (rxring) == self->heads[r])
			continue;
		fpga_pkt* pkt = (fpga_pkt*) ifring_buf (
				rxring, self->heads[r]);
		uint16_t cur_fseq = frame_seq (pkt);
		uint16_t fseq_gap = cur_fseq - self->prev_fseq - 1;
		if (fseq_gap <= missed)
		{
			next_ring_id = r;
			missed = fseq_gap;
			if (fseq_gap == 0)
				break;
		}
	}
	/*
	 * We should never have received a WAKEUP if there are no new
	 * packets, but sometimes we do, why?
	 */
	if (next_ring_id < 0)
	{
#ifdef FULL_DBG
		self->dbg_stats.wakeups_false++;
#endif
		self->busy = 0;
		return 0;
	}

	int rc = s_task_dispatch (self, loop, next_ring_id, missed);
	if (self->error)
	{
		self->active = 0;
		return -1;
	}
	/* TO DO: check the rest of the rings, depending on rc */

	self->busy = 0;
	return 0;
}

/*
 * Registered with the coordinator's loop. Receives SIG_DIED sent by a task and
 * terminates the coordinator's loop. 
 */
static int
s_die_hn (zloop_t* loop, zsock_t* reader, void* ignored)
{
	dbg_assert (ignored == NULL);

#ifdef FULL_DBG
	/* Catch bugs by receiving a message and asserting it's a signal.
	 * zsock_wait discards messages until a signal arrives. */
	zmsg_t* msg = zmsg_recv (reader);
	if (msg == NULL)
	{
		s_msg (0, LOG_DEBUG, 0, "Receive interrupted");
		return -1;
	}
	int sig = zmsg_signal (msg);
	zmsg_destroy (&msg);
	dbg_assert (sig >= 0);
#else
	int sig = zsock_wait (reader);
	if (sig == -1)
	{
		s_msg (0, LOG_DEBUG, 0, "Receive interrupted");
		return -1;
	}
#endif

	if (sig == SIG_DIED)
	{
		s_msg (0, LOG_DEBUG, 0,
			"Task thread encountered an error");
		return -1;
	}
	assert (0); /* we only deal with SIG_DIED  */
}

/*
 * A generic body for a task.
 */
static void
s_task_shim (zsock_t* pipe, void* self_)
{
	dbg_assert (self_ != NULL);
	zsock_signal (pipe, 0); /* zactor_new will wait for this */

	int rc;
	task_t* self = (task_t*) self_;
	dbg_assert (self->pkt_handler != NULL);
	dbg_assert (self->data == NULL);
	dbg_assert (self->ifd != NULL);
	dbg_assert (self->frontend == NULL);
	dbg_assert (self->id > 0);
	for (int r = 0; r < NUM_RINGS; r++)
		dbg_assert (self->heads[r] == 0);
	dbg_assert (self->prev_fseq == 0);
	dbg_assert (self->prev_pseq_mca == 0);
	dbg_assert (self->prev_pseq_tr == 0);
	dbg_assert (self->prev_pseq_pls == 0);
	dbg_assert (self->error == 0);
	dbg_assert (self->busy == 0);
	dbg_assert (self->active == 0);
	
	zloop_t* loop = zloop_new ();
	/* Only the coordinator thread should get interrupted, we wait for
	 * SIG_STOP. */
#if (CZMQ_VERSION_MAJOR > 3)
	zloop_set_nonstop (loop, 1);
#else
	zloop_ignore_interrupts (loop);
#endif

	// s_msg (0, LOG_DEBUG, self->id, "Simulating error");
	// self->error = 1;
	// goto cleanup;

	/* Open the public interface */
	if (self->front_addr != NULL)
	{
		self->frontend = zsock_new (self->front_type);
		if (self->frontend == NULL)
		{
			s_msg (errno, LOG_ERR, self->id,
				"Could not open the public interface");
			self->error = 1;
			goto cleanup;
		}
		rc = zsock_attach (self->frontend, self->front_addr, 1);
		if (rc)
		{
			s_msg (errno, LOG_ERR, self->id,
				"Could not bind the public interface");
			self->error = 1;
			goto cleanup;
		}
		s_msgf (0, LOG_INFO, self->id,
			"Listening on port(s) %s", self->front_addr);
	}
	/* Register the readers */
	rc = zloop_reader (loop, pipe, s_sig_hn, self);
	if (self->client_handler != NULL)
	{
		dbg_assert (self->frontend != NULL);
		rc |= zloop_reader (loop, self->frontend,
				self->client_handler, self);
	}
	if (rc)
	{
		s_msg (errno, LOG_ERR, self->id,
			"Could not register the zloop readers");
		self->error = 1;
		goto cleanup;
	}

	/* Call initializer */
	if (self->data_init != NULL)
	{
		rc = self->data_init (self);
		if (rc)
		{
			s_msg (errno, LOG_ERR, self->id,
				"Could not initialize thread data");
			self->error = 1;
			goto cleanup;
		}
	}

	s_msg (0, LOG_DEBUG, self->id, "Polling");
	zsock_signal (pipe, SIG_INIT); /* task_new will wait for this */
	
	if (self->autoactivate)
		s_task_activate (self);
	rc = zloop_start (loop);
	dbg_assert (rc == -1); /* we don't get interrupted */

cleanup:
	self->active = 0;
	/*
	 * zactor_destroy waits for a signal from s_thread_shim (see DEV
	 * NOTES). To avoid returning from zactor_destroy prematurely, we only
	 * send SIG_DIED if we exited due to an error on our part (in one of
	 * the handlers).
	 */
	if (self->error)
		zsock_signal (pipe, SIG_DIED);

	if (self->data_fin != NULL)
	{
		rc = self->data_fin (self);
		if (rc)
		{
			s_msg (errno, LOG_ERR, self->id,
				"Could not cleanup thread data");
		}
		dbg_assert (self->data == NULL);
	}
	zloop_destroy (&loop);
	zsock_destroy (&self->frontend);
	s_msg (0, LOG_DEBUG, self->id, "Done");
#ifdef FULL_DBG
	s_msgf (0, LOG_DEBUG, self->id,
		"Woken up %lu times, %lu when not active, "
		"%lu when no new packets, dispatched "
		"%lu rings, %lu packets received, %lu missed",
		self->dbg_stats.wakeups,
		self->dbg_stats.wakeups_inactive,
		self->dbg_stats.wakeups_false,
		self->dbg_stats.rings_dispatched,
		self->dbg_stats.pkts.rcvd,
		self->dbg_stats.pkts.missed
		);
#endif
}

/*
 * Initializes a task_t and starts a new thread using zactor_new.
 * Registers the task's back end of the pipe with the coordinator's loop.
 * Returns 0 on success, -1 on error.
 */
static int
s_task_start (ifdesc* ifd, task_t* self)
{
	dbg_assert (self != NULL);
	dbg_assert (ifd != NULL);

	self->ifd = ifd;
	dbg_assert (if_rxrings (ifd) == NUM_RINGS);

	/* Start the thread, will block until the handler signals */
	self->shim = zactor_new (s_task_shim, self);
	dbg_assert (self->shim != NULL);
	/* zactor_new does not check the signal, so no way to know if there was
	 * an error. As a workaroung the task thread will send a second signal
	 * when it is ready (or when it fails) and we wait for it here. */
	int rc = zsock_wait (self->shim);
	if (rc == SIG_DIED)
	{
		s_msg (0, LOG_DEBUG, self->id,
			"Task thread failed to initialize");
		return -1;
	}
	dbg_assert (rc == SIG_INIT);
	s_msg (0, LOG_DEBUG, self->id, "Task thread initialized");

	return 0;
}

/*
 * This is to be used instead of zactor_destroy, as a workaround for not
 * setting a custom destructor.
 */
static void
s_task_stop (task_t* self)
{
	dbg_assert (self != NULL);
	if (self->shim == NULL)
	{
		s_msg (0, LOG_DEBUG, self->id,
			"Task had already exited");
		return;
	}

	zsock_set_sndtimeo (self->shim, 0);
	zsock_signal (self->shim, SIG_STOP);
	/* Wait for the final signal from zactor's s_thread_shim.
	 * zactor_destroy will send "$TERM" which will be ignored; not
	 * a problem. */
	zactor_destroy (&self->shim);
}

/*
 * Synchronizes the task's head with the ring's head and sets active to true.
 */
static inline void
s_task_activate (task_t* self)
{
	dbg_assert (self != NULL);
	for (int r = 0; r < NUM_RINGS; r++)
	{
		ifring* rxring = if_rxring (self->ifd, r);
		self->heads[r] = ifring_head (rxring);
	}
	self->active = 1;
}

/*
 * Loops over the given ring until either reaching the tail or seeing
 * a discontinuity in frame sequence. For each buffer calls the task's
 * pkt_handler.
 * Returns ... TO DO
 */
static int s_task_dispatch (task_t* self, zloop_t* loop,
		uint16_t ring_id, uint16_t missed)
{
	dbg_assert (self != NULL);
	dbg_assert (loop != NULL);
#ifdef FULL_DBG
	self->dbg_stats.rings_dispatched++;
	self->dbg_stats.pkts.missed += missed;
#endif

	ifring* rxring = if_rxring (self->ifd, ring_id);
	/*
	 * First exec of the loop uses the head from the last time
	 * dispatch was called with this ring_id.
	 */
	uint16_t fseq_gap = missed;
	do
	{
		fpga_pkt* pkt = (fpga_pkt*) ifring_buf (
			rxring, self->heads[ring_id]);
		dbg_assert (pkt != NULL);
#ifdef FULL_DBG
		self->dbg_stats.pkts.rcvd++;
#endif

		/* TO DO: check packet */
		uint16_t len = ifring_len (rxring, self->heads[ring_id]);
		uint16_t plen = pkt_len (pkt);
		if (plen > len)
		{ /* drop the frame */
			s_msgf (0, LOG_DEBUG, self->id,
				"Packet too long (header says %hu, "
				"ring slot is %hu)", plen, len);
			return 0;
		}
		dbg_assert (plen <= MAX_FPGA_FRAME_LEN);
		int rc = self->pkt_handler (loop, pkt, plen, fseq_gap, self);

		uint16_t cur_fseq = frame_seq (pkt);
		fseq_gap = cur_fseq - self->prev_fseq - 1;

		self->prev_fseq = cur_fseq;
		if (is_mca (pkt))
			self->prev_pseq_mca = proto_seq (pkt);
		else if (is_trace (pkt))
			self->prev_pseq_tr = proto_seq (pkt);
		else
			self->prev_pseq_pls = proto_seq (pkt);

		self->heads[ring_id] = ifring_following (
				rxring, self->heads[ring_id]);

		/* TO DO: return codes */
		if (rc)
			return 0; /* pkt_handler doesn't want more */

		if (fseq_gap > 0)
			return 0;

	} while (self->heads[ring_id] != ifring_tail (rxring));

	return 0;
}

/* ------------------------------------------------------------------------- */
/* --------------------------- SAVE-TO-FILE TASK --------------------------- */
/* ------------------------------------------------------------------------- */

/*
 * Called when a client sends a request on the REP socket. For valid requests
 * of status, opens the file and send the reply. For valid requests to save,
 * opens the file and marks the task as active, so that next time the
 * coordinator reads new packets it will send a SIG_WAKEUP.
 */
static int
s_task_save_req_hn (zloop_t* loop, zsock_t* reader, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;
	dbg_assert ( ! self->busy );
	dbg_assert ( ! self->active );
	struct s_task_save_data_t* sjob =
		(struct s_task_save_data_t*) self->data;
	uint8_t job_mode;

	char* filename;
	int rc = zsock_recv (reader, REQ_PIC, &filename,
		&sjob->max_ticks, &job_mode);
	if (rc == -1)
	{ /* would also return -1 if picture contained a pointer (p) or a null
	   * frame (z) but message received did not match this signature; this
	   * is irrelevant in this case */
		s_msg (0, LOG_DEBUG, self->id,
			"Receive interrupted");
		self->error = 1;
		return -1;
	}

	if (filename == NULL || job_mode > 1)
	{
		s_msg (0, LOG_INFO, self->id,
			"Received a malformed request");
		zsock_send (reader, REP_PIC, REQ_FAIL, 0, 0, 0, 0);
		return 0;
	}

	int checkonly = (sjob->max_ticks == 0);
	if (checkonly)
	{
		s_msgf (0, LOG_INFO, self->id,
			"Received request for status of '%s'",
			filename);
	}
	else
	{
		s_msgf (0, LOG_INFO, self->id,
			"Received request to write %lu ticks to '%s'",
			sjob->max_ticks, filename);
	}

	/* Check if filename is allowed and get the realpath. */
	sjob->filename = s_task_save_canonicalize_path (
			filename, checkonly, self->id);
	if (sjob->filename == NULL)
	{
		if ( ! checkonly )
		{
			s_msg (errno, LOG_INFO, self->id,
				"Filename is not valid");
		}
		zstr_free (&filename); /* nullifies the pointer */
		zsock_send (reader, REP_PIC, REQ_FAIL, 0, 0, 0, 0);
		return 0;
	}
	dbg_assert (sjob->filename != NULL);
	zstr_free (&filename); /* nullifies the pointer */

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
	if (checkonly)
	{ /* status */
		fmode = O_RDONLY;
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

	rc = s_task_save_open (sjob, fmode);
	if (rc == -1)
	{
		if (errno != exp_errno)
		{
			s_msgf (errno, LOG_ERR, self->id,
				"Could not open file %s",
				sjob->filename);
		}
		s_msgf (0, LOG_INFO, self->id,
			"Not %s file", (fmode & O_RDWR ) ?
				"writing to" : "reading from");
		zsock_send (reader, REP_PIC, REQ_FAIL, 0, 0, 0, 0);
		s_task_save_close (sjob);
		return 0;
	}
	s_msgf (0, LOG_INFO, self->id, "Opened file %s for %s",
		sjob->filename,
		(fmode & O_RDWR ) ? "writing" : "reading");

	if (checkonly)
	{ /* just read in stats and send reply */
		s_task_save_read  (sjob);
		s_task_save_send  (sjob, self->frontend);
		s_task_save_close (sjob);
		return 0;
	}

	/* Disable polling on the reader until the job is done */
	zloop_reader_end (loop, reader);
	s_task_activate (self);
	return 0;
}

/*
 * Saves packets to a file. plen is the frame length. Will drop frames that say
 * packet is longer than this. Will not write more than what the frame header
 * says.
 */
static int
s_task_save_pkt_hn (zloop_t* loop, fpga_pkt* pkt, uint16_t plen,
		uint16_t missed, task_t* self)
{
	dbg_assert (self != NULL);

	struct s_task_save_data_t* sjob =
		(struct s_task_save_data_t*) self->data;
	dbg_assert (sjob->filename != NULL);
	dbg_assert (sjob->aios.aio_fildes != -1);
	dbg_assert (self->active);

#ifdef FULL_DBG
	if (sjob->bufzone.enqueued + sjob->bufzone.waiting >
		TSAVE_BUFSIZE - MAX_FPGA_FRAME_LEN)
	{
		s_msgf (0, LOG_DEBUG, self->id,
			"Waiting: %lu, in queue: %lu free: %ld, "
			"previously waiting: %lu, previously enqueued: %lu",
			sjob->bufzone.waiting, sjob->bufzone.enqueued,
			(long)(TSAVE_BUFSIZE - sjob->bufzone.waiting
			- sjob->bufzone.enqueued),
			sjob->prev_waiting, sjob->prev_enqueued);
		self->error = 1;
		return -1;
	}
#endif
	dbg_assert (sjob->bufzone.enqueued + sjob->bufzone.waiting <=
		TSAVE_BUFSIZE - MAX_FPGA_FRAME_LEN);
	dbg_assert (sjob->bufzone.cur >= sjob->bufzone.base);
	dbg_assert (sjob->bufzone.tail >= sjob->bufzone.base);
	dbg_assert (sjob->bufzone.cur < sjob->bufzone.ceil);
	dbg_assert (sjob->bufzone.tail + sjob->bufzone.enqueued <=
		sjob->bufzone.ceil);
	dbg_assert (sjob->bufzone.cur < sjob->bufzone.tail ||
		sjob->bufzone.cur >= sjob->bufzone.tail + sjob->bufzone.enqueued);
	dbg_assert (sjob->bufzone.cur == sjob->bufzone.tail
		+ sjob->bufzone.enqueued + sjob->bufzone.waiting -
		((sjob->bufzone.cur < sjob->bufzone.tail) ? TSAVE_BUFSIZE : 0));

	/* TO DO: save err flags */
	/* Update statistics. Size is updated in batches as write operations
	 * finish. */
	uint16_t cur_fseq = frame_seq (pkt);
	if (sjob->st.frames > 0)
		sjob->st.frames_lost += missed;
		// sjob->st.frames_lost += (uint16_t)(cur_fseq
		//                 - self->prev_fseq - 1);
#ifdef FULL_DBG
	if (sjob->st.frames_lost != 0)
	{
		s_msgf (0, LOG_DEBUG, self->id, "Lost frames: %hu -> %hu",
			self->prev_fseq, cur_fseq);
		s_dump_buf (sjob->prev_hdr, FPGA_HDR_LEN);
		s_dump_buf ((void*)pkt, FPGA_HDR_LEN);
		self->error = 1;
		return -1;
	}
	memcpy (sjob->prev_hdr, pkt, FPGA_HDR_LEN);
#endif

	sjob->st.frames++;
	if (is_tick (pkt))
		sjob->st.ticks++;

	/* Wrap cursor if needed */
	int reserve = plen - (sjob->bufzone.ceil - sjob->bufzone.cur);
	if (likely (reserve < 0))
	{
		memcpy (sjob->bufzone.cur, pkt, plen); 
		sjob->bufzone.cur += plen;
	}
	else
	{
		memcpy (sjob->bufzone.cur, pkt, plen - reserve); 
		if (reserve > 0)
			memcpy (sjob->bufzone.base,
				(char*)pkt + plen - reserve, reserve); 
		sjob->bufzone.cur = sjob->bufzone.base + reserve;
	}
	sjob->bufzone.waiting += plen;

	if (sjob->st.ticks == sjob->max_ticks)
		self->active = 0;

#if 1 /* 0 to skip writing */
	/* Trye to queue next batch but don't force */
	int jobrc = s_task_save_queue (sjob, 0);
	/* If there is no space for a full frame, force write until there is.
	 * If we are finalizingm wait for all bytes to be written. */
	while ( ( sjob->bufzone.enqueued + sjob->bufzone.waiting >
		TSAVE_BUFSIZE - MAX_FPGA_FRAME_LEN || ! self->active )
		&& jobrc == EINPROGRESS )
	{
		jobrc = s_task_save_queue (sjob, 1);
	}

	if ( ! self->active )
		dbg_assert (jobrc != EINPROGRESS);

	if (jobrc == -1)
	{
		/* TO DO: how to handle errors */
		s_msg (errno, LOG_ERR, self->id,
			"Could not write to file");
		self->active = 0;
	}
	else if (jobrc == -2)
	{
		/* TO DO: how to handle errors */
#ifdef FULL_DBG
		s_msgf (0, LOG_ERR, self->id,
			"Queued %lu bytes, wrote %lu",
			sjob->bufzone.enqueued, sjob->last_written);
#else
		s_msg (0, LOG_ERR, self->id,
			"Wrote unexpected number of bytes");
#endif
		self->active = 0;
	}
#else /* skip writing */
	sjob->st.size += sjob->bufzone.waiting;
	sjob->bufzone.waiting = 0;
	sjob->bufzone.tail = sjob->bufzone.cur;
#endif /* skip writing */

	dbg_assert (sjob->bufzone.enqueued + sjob->bufzone.waiting <=
		TSAVE_BUFSIZE - MAX_FPGA_FRAME_LEN);

	if ( ! self->active )
	{
		/* TO DO: truncate file if overwriting */
		s_msgf (0, LOG_INFO, self->id,
			"Finished writing %lu ticks to file %s",
			sjob->st.ticks, sjob->filename);
#ifdef FULL_DBG
		s_msgf (0, LOG_DEBUG, self->id,
			"Wrote %lu packets in %lu batches (%lu repeated, "
			"%lu cleared all)", sjob->st.frames,
			sjob->batches, sjob->failed_batches, sjob->num_cleared);
#endif
		/* TO DO: check rc */
		s_task_save_write (sjob);
		s_task_save_send  (sjob, self->frontend);
		s_task_save_close (sjob);
		/* Enable polling on the reader */
		int rc = zloop_reader (loop, self->frontend,
				self->client_handler, self);
		if (rc == -1)
		{
			s_msg (errno, LOG_ERR, self->id,
				"Could not re-enable the zloop reader");
			self->error = 1;
		}
		return -1;
	}

	return 0;
}

static int
s_task_save_init (task_t* self)
{
	dbg_assert (*(TSAVE_ROOT + strlen (TSAVE_ROOT) - 1) == '/');
	dbg_assert (sizeof (struct s_task_save_stats_t) == TSAVE_SOFFSET);
	dbg_assert (self != NULL);

	static struct s_task_save_data_t sjob;
	sjob.aios.aio_sigevent.sigev_notify = SIGEV_NONE;
	sjob.aios.aio_fildes = -1;

	void* buf = mmap (NULL, TSAVE_BUFSIZE, PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == (void*)-1)
	{
		s_msgf (errno, LOG_ERR, self->id,
			"Cannot mmap %lu bytes", TSAVE_BUFSIZE);
		return -1;
	}
	sjob.bufzone.base = sjob.bufzone.tail =
		sjob.bufzone.cur = (unsigned char*) buf;
	sjob.bufzone.ceil = sjob.bufzone.base + TSAVE_BUFSIZE;

	self->data = &sjob;
	return 0;
}

static int
s_task_save_fin (task_t* self)
{
	dbg_assert (self != NULL);
	dbg_assert ( ! self->active );
	struct s_task_save_data_t* sjob =
		(struct s_task_save_data_t*) self->data;
	dbg_assert (sjob != NULL);
	if (sjob->aios.aio_fildes != -1)
	{
		dbg_assert (sjob->filename != NULL);
		s_task_save_write (sjob);
		s_task_save_send  (sjob, self->frontend);
		s_task_save_close (sjob);
	}

	/* Unmap bufzone */
	if (sjob->bufzone.base != NULL)
	{
		munmap (sjob->bufzone.base, TSAVE_BUFSIZE);
		sjob->bufzone.base = NULL;
	}

	self->data = NULL;
	return 0;
}

/*
 * Opens the file. Returns 0 on success, -1 on error.
 */
static int
s_task_save_open (struct s_task_save_data_t* sjob, mode_t fmode)
{
	dbg_assert (sjob != NULL);
	dbg_assert (sjob->filename != NULL);
	dbg_assert (sjob->aios.aio_fildes == -1);
#ifdef FULL_DBG
	dbg_assert (sjob->prev_enqueued == 0);
	dbg_assert (sjob->prev_waiting == 0);
	dbg_assert (sjob->batches == 0);
	dbg_assert (sjob->failed_batches == 0);
	dbg_assert (sjob->num_cleared == 0);
	dbg_assert (sjob->last_written == 0);
#endif
	dbg_assert (sjob->st.ticks == 0);
	dbg_assert (sjob->st.size == 0);
	dbg_assert (sjob->st.frames == 0);
	dbg_assert (sjob->st.frames_lost == 0);
	dbg_assert (sjob->st.errors == 0);

	/* Open the file */
	sjob->aios.aio_fildes = open (sjob->filename, fmode,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (sjob->aios.aio_fildes == -1)
		return -1;

	int rc;
	rc = lseek (sjob->aios.aio_fildes, TSAVE_SOFFSET, 0);
	if (rc != TSAVE_SOFFSET)
		return -1;

	return 0;
}

/*
 * Queue the next batch for aio_write-ing.
 * If force is true, will suspend if file is not ready for writing.
 * Always calls aio_return for previous job. Calls aio_return if waiting for
 * new job.
 * 
 * Returns 0 if no bytes left to write.
 * Returns EINPROGRESS on successful queue, or if force is false and file is
 * not ready.
 * Returns -1 on error.
 * Returns -2 if number of bytes written as reported by aio_return is
 * unexpected.
 */
static int
s_task_save_queue (struct s_task_save_data_t* sjob, bool force)
{
	/* If there was no previous job, no need to do checks. */
	if (sjob->bufzone.enqueued == 0)
		goto prepare_next;

	/* ----------------------------------------------------------------- */
	/* Check if ready. */
	int rc = aio_error (&sjob->aios);
	if ( ! force && rc == EINPROGRESS )
		return EINPROGRESS;

	/* Suspend while ready. */
	if ( rc == EINPROGRESS )
	{
		const struct aiocb* aiol = &sjob->aios;
		rc = aio_suspend (&aiol, 1, NULL);
		if (rc == -1)
			return -1;
		rc = aio_error (&sjob->aios);
	}

	if (rc)
	{
		dbg_assert (rc != ECANCELED && rc != EINPROGRESS);
		errno = rc; /* aio_error does not set it */
		return -1;
	}

	/* Check completion status. */
	ssize_t wrc = aio_return (&sjob->aios);
	if (wrc == -1 && errno == EAGAIN)
	{
#ifdef FULL_DBG
		sjob->failed_batches++;
#endif
		goto queue_as_is; /* requeue previous batch */
	}

	if (wrc == -1)
		return -1; /* an error other than EAGAIN */
	if ((size_t)wrc != sjob->bufzone.enqueued)
	{
		dbg_assert (sjob->bufzone.enqueued > 0);
#ifdef FULL_DBG
		sjob->last_written = wrc;
#endif
		return -2;
	}

	/* ----------------------------------------------------------------- */
prepare_next:
#ifdef FULL_DBG
	sjob->batches++;
	sjob->prev_waiting = sjob->bufzone.waiting;
	sjob->prev_enqueued = sjob->bufzone.enqueued;
#endif

	/* Increase file size by number of bytes written. */
	sjob->st.size += sjob->bufzone.enqueued;

	sjob->bufzone.tail += sjob->bufzone.enqueued;
	if (sjob->bufzone.tail == sjob->bufzone.ceil)
		sjob->bufzone.tail = sjob->bufzone.base;
	dbg_assert (sjob->bufzone.tail < sjob->bufzone.ceil);

	/* If cursor had wrapped around, queue until the end of the
	 * bufzone. When done, tail will move to ceil, we handle
	 * this above. */
	if (unlikely (sjob->bufzone.cur < sjob->bufzone.tail))
		sjob->bufzone.enqueued = sjob->bufzone.ceil
			- sjob->bufzone.tail;
	else
		sjob->bufzone.enqueued = sjob->bufzone.cur
			- sjob->bufzone.tail;

	dbg_assert (sjob->bufzone.waiting >= sjob->bufzone.enqueued);
	sjob->bufzone.waiting -= sjob->bufzone.enqueued;

	dbg_assert (sjob->bufzone.waiting == 0 || sjob->bufzone.tail
			+ sjob->bufzone.enqueued == sjob->bufzone.ceil);

	/* ----------------------------------------------------------------- */
queue_as_is:
	/* Check if all waiting bytes have been written. */
	if (sjob->bufzone.enqueued == 0)
	{
#ifdef FULL_DBG
		sjob->num_cleared++;
#endif
		return 0;
	}

	sjob->aios.aio_offset = sjob->st.size + TSAVE_SOFFSET;
	sjob->aios.aio_buf = sjob->bufzone.tail;
	sjob->aios.aio_nbytes = sjob->bufzone.enqueued;
	do
	{
		rc = aio_write (&sjob->aios);
	} while (rc == -1 && errno == EAGAIN);
	if (rc == -1)
		return -1; /* an error other than EAGAIN */
	return EINPROGRESS;
}

/*
 * Reads stats previously saved to file. Used when client requests a status for
 * filename.
 */
static int
s_task_save_read (struct s_task_save_data_t* sjob)
{
	dbg_assert (sjob != NULL);
	dbg_assert (sjob->filename != NULL);
	dbg_assert (sjob->aios.aio_fildes != -1);
	dbg_assert (sjob->max_ticks == 0);
#ifdef FULL_DBG
	dbg_assert (sjob->prev_enqueued == 0);
	dbg_assert (sjob->prev_waiting == 0);
	dbg_assert (sjob->batches == 0);
	dbg_assert (sjob->failed_batches == 0);
	dbg_assert (sjob->num_cleared == 0);
	dbg_assert (sjob->last_written == 0);
#endif
	dbg_assert (sjob->st.ticks == 0);
	dbg_assert (sjob->st.size == 0);
	dbg_assert (sjob->st.frames == 0);
	dbg_assert (sjob->st.frames_lost == 0);
	dbg_assert (sjob->st.errors == 0);

	off_t rc = lseek (sjob->aios.aio_fildes, 0, 0);
	if (rc)
		return -1;
	
	rc = read (sjob->aios.aio_fildes, &sjob->st, TSAVE_SOFFSET);
	if (rc != TSAVE_SOFFSET)
		return -1;
	
	return 0;
}

/*
 * Writes stats to a currently open file. Used right before closing it.
 */
static int
s_task_save_write (struct s_task_save_data_t* sjob)
{
	dbg_assert (sjob != NULL);
	dbg_assert (sjob->filename != NULL);
	dbg_assert (sjob->aios.aio_fildes != -1);

	off_t rc = lseek (sjob->aios.aio_fildes, 0, 0);
	if (rc)
		return -1;
	
	rc = write (sjob->aios.aio_fildes, &sjob->st, TSAVE_SOFFSET);
	if (rc != TSAVE_SOFFSET)
		return -1;
	
	return 0;
}

/*
 * Sends the statistics to the client.
 */
static int
s_task_save_send  (struct s_task_save_data_t* sjob, zsock_t* frontend)
{
	dbg_assert (sjob != NULL);
	dbg_assert (sjob->filename != NULL);
	/* When the file is closed, the stats are reset.
	 * Call _send before _close. */
	dbg_assert (sjob->aios.aio_fildes != -1);

	return zsock_send (frontend, REP_PIC, REQ_OK, 
			sjob->st.ticks,
			sjob->st.size,
			sjob->st.frames,
			sjob->st.frames_lost);
}

/*
 * Closes the file descriptor, nullifies and resets stats.
 */
static void
s_task_save_close (struct s_task_save_data_t* sjob)
{
	dbg_assert (sjob != NULL);

	if (sjob->aios.aio_fildes >= 0)
	{
		close (sjob->aios.aio_fildes);
		sjob->aios.aio_fildes = -1;
	}

	sjob->filename = NULL; /* points to a static string */
	sjob->max_ticks = 0;
#ifdef FULL_DBG
	sjob->prev_enqueued = 0;
	sjob->prev_waiting = 0;
	sjob->batches = 0;
	sjob->failed_batches = 0;
	sjob->num_cleared = 0;
	sjob->last_written = 0;
#endif
	sjob->st.ticks = 0;
	sjob->st.size = 0;
	sjob->st.frames = 0;
	sjob->st.frames_lost = 0;
	sjob->st.errors = 0;
}

/*
 * Prepends TSAVE_ROOT to filename and canonicalizes the path via realpath.
 * If checkonly is false, creates any missing parent directories.
 * On success returns a pointer to a statically allocated string, caller
 * must not free it.
 * Returns NULL on error (including if checkonly is true and the filename does
 * not exist).
 * If NULL is returned because the filename is not allowed by us (i.e. outside
 * of TSAVE_ROOT or ends with a slash) errno should be 0.
 */
static char*
s_task_save_canonicalize_path (const char* filename, int checkonly, int task_id)
{
	dbg_assert (filename != NULL);
	errno = 0;
	size_t len = strlen (filename);
	if (len == 0)
	{
		s_msg (0, LOG_DEBUG, task_id,
			"Filename is empty");
		return NULL;
	}

#ifdef TSAVE_ONLYFILES
	if (filename[len - 1] == '/')
	{
		s_msg (0, LOG_DEBUG, task_id,
			"Filename ends with /");
		return NULL;
	}
#endif

	/* Only one thread should use this, so static storage is fine. */
	static char finalpath[PATH_MAX];

	char buf[PATH_MAX];
	memset (buf, 0, PATH_MAX);
	snprintf (buf, PATH_MAX, "%s%s", TSAVE_ROOT, filename);

	/* Check if the file exists first. */
	errno = 0;
	char* rs = realpath (buf, finalpath);
	if (rs)
	{
		errno = 0;
		dbg_assert (rs == finalpath);
		if ( memcmp (finalpath, TSAVE_ROOT, strlen (TSAVE_ROOT)) != 0)
		{
			s_msgf (0, LOG_DEBUG, task_id,
				"Resolved to %s, outside of root",
				finalpath);
			return NULL; /* outside of root */
		}
		return finalpath;
	}
	if (checkonly)
	{
		s_msg (0, LOG_DEBUG, task_id,
			"File doesn't exist");
		return NULL;
	}

	/*
	 * We proceed only if some of the directories are missing, i.e. errno
	 * is ENOENT.
	 * errno is ENOTDIR only when a component of the parent path exists but
	 * is not a directory. If filename ends with a / the part before the
	 * last slash is also considered a directory, so will return ENOTDIR if
	 * it is an existing file, but ENOENT if it doesn't exist.
	 */
	if (errno != ENOENT)
		return NULL;

	/* Start from the top-most component (after TSAVE_ROOT) and create
	 * directories as needed. */
	memset (buf, 0, PATH_MAX);
	strcpy (buf, TSAVE_ROOT);

	const char* cur_seg = filename;
	const char* next_seg = NULL;
	len = strlen (buf);
	while ( (next_seg = strchr (cur_seg, '/')) != NULL)
	{
		if (cur_seg[0] == '/')
		{ /* multiple consecutive slashes */
			cur_seg++;
			continue;
		}

		/* copy leading slash of next_seg at the end */
		dbg_assert (len < PATH_MAX);
		if (len + next_seg - cur_seg + 1 >= PATH_MAX)
		{
			s_msg (0, LOG_DEBUG, task_id,
				"Filename too long");
			return NULL;
		}
		strncpy (buf + len, cur_seg, next_seg - cur_seg + 1);
		len += next_seg - cur_seg + 1;
		dbg_assert (len == strlen (buf));

		errno = 0;
		/* FIX: check that realpath is not outside of TSAVE_ROOT before
		 * attempting to create. */
		int rc = mkdir (buf, 0777);
		if (rc && errno != EEXIST)
			return NULL; /* don't handle other errors */

		cur_seg = next_seg + 1; /* skip over leading slash */
	}

	/* Canonicalize the directory part */
	rs = realpath (buf, finalpath);
	dbg_assert (rs != NULL); /* this shouldn't happen */
	dbg_assert (rs == finalpath);

	/* Add the base filename (realpath removes the trailing slash) */
#ifdef TSAVE_ONLYFILES
	dbg_assert (strlen (cur_seg) > 0);
#else
	/* TO DO: generate a random filename is none is given */
#endif
	len = strlen (finalpath);
	if (strlen (cur_seg) + len >= PATH_MAX)
	{
		s_msg (0, LOG_DEBUG, task_id,
				"Filename too long");
		return NULL;
	}

	snprintf (finalpath + len, PATH_MAX - len, "/%s", cur_seg);
	errno = 0;
	if ( memcmp (finalpath, TSAVE_ROOT, strlen (TSAVE_ROOT)) != 0)
	{
		s_msgf (0, LOG_DEBUG, task_id,
				"Resolved to %s, outside of root",
				finalpath);
		return NULL; /* outside of root */
	}

	return finalpath;
}

/* ------------------------------------------------------------------------- */
/* --------------------------- PUBLISH HIST TASK --------------------------- */
/* ------------------------------------------------------------------------- */

/*
 * Accumulates MCA frames and sends them out as soon as the last one is
 * received. It aborts the whole histogram if an MCA frame is lost or if extra
 * frames are received (i.e. the size field appears to small).
 */
static int
s_task_hist_pkt_hn (zloop_t* loop, fpga_pkt* pkt, uint16_t plen,
		uint16_t missed, task_t* self)
{
	dbg_assert (self != NULL);

	if ( ! is_mca (pkt) )
		return 0;

	struct s_task_hist_data_t* hist =
		(struct s_task_hist_data_t*) self->data;

	if ( ! is_header (pkt) )
	{
		if (hist->discard) 
			return 0;

		/* Check protocol sequence */
		uint16_t cur_pseq = proto_seq (pkt);
		if ((uint16_t)(cur_pseq - self->prev_pseq_mca) != 1)
		{
			s_msgf (0, LOG_INFO, self->id,
				"Frame out of protocol sequence: %hu -> %hu",
				self->prev_pseq_mca, cur_pseq);
			hist->discard = 1;
			return 0;
		}
	}
	else
	{
		if (hist->cur_nbins > 0)
		{
			s_msgf (0, LOG_WARNING, self->id,
				"Received new header frame while waiting for "
				"%d more bins", hist->nbins - hist->cur_nbins);
			hist->discard = 1;
		}

		if (hist->discard)
		{
			/* Drop the previous one. */
			hist->size = 0;
			hist->nbins = 0;
			hist->cur_size = 0;
			hist->cur_nbins = 0;
			hist->discard = 0;
#ifdef FULL_DBG
			hist->dropped++;
			s_msgf (0, LOG_DEBUG, self->id,
				"Discarded %lu out of %lu histograms so far",
				hist->dropped, hist->dropped + hist->published);
#endif
		}

		dbg_assert (hist->nbins == 0);
		dbg_assert (hist->size == 0);
		dbg_assert (hist->cur_nbins == 0);
		dbg_assert (hist->cur_size == 0);
		dbg_assert ( ! hist->discard );

		/* Inspect header */
		hist->nbins = mca_num_allbins (pkt);
		hist->size  = mca_size (pkt);

		/* TO DO: incorporate this into generic packet check */
#if 0 /* until 'size' field calculation bug is fixed */
#ifdef FULL_DBG
		if (hist->size != hist->nbins * BIN_LEN + MCA_HDR_LEN)
		{
			s_msgf (0, LOG_WARNING, self->id,
				"Size field (%d B) does not match "
				"number of bins (%d)",
				hist->size, hist->nbins);
			hist->discard = 1;
			return 0;
		}
#endif
#endif
	}
	dbg_assert ( ! hist->discard );

	hist->cur_nbins += mca_num_bins (pkt);
	if (hist->cur_nbins > hist->nbins)
	{
		s_msgf (0, LOG_WARNING, self->id,
			"Received extra bins: expected %d, so far got %d",
			hist->nbins, hist->cur_nbins);
		hist->discard = 1;
		return 0;
	}

	/* Copy frame */
	uint16_t fsize = pkt_len (pkt) - FPGA_HDR_LEN;
	dbg_assert (hist->cur_size <= THIST_MAXSIZE - fsize);
	memcpy (hist->buf + hist->cur_size,
		(char*)pkt + FPGA_HDR_LEN, fsize);

	hist->cur_size += fsize;

	if (hist->cur_nbins == hist->nbins) 
	{
#if 0 /* FIX, see above */
		dbg_assert (hist->cur_size == hist->size);
#endif

		/* Send the histogram */
#ifdef FULL_DBG
		hist->published++;
		// s_msgf (0, LOG_DEBUG, self->id,
		//         "Publishing an %u-byte long histogram",
		//         hist->cur_size);
		int rc = zmq_send (zsock_resolve (self->frontend),
			hist->buf, hist->cur_size, 0);
		if (rc == -1)
		{
			s_msg (errno, LOG_ERR, self->id,
				"Cannot send the histogram");
			self->error = 1;
			return -1;
		}
		if (rc != hist->cur_size)
		{
			s_msgf (errno, LOG_ERR, self->id,
				"Histogram is %lu bytes long, sent %u",
				hist->cur_size, rc);
			self->error = 1;
			return -1;
		}
#else
		zmq_send (zsock_resolve (self->frontend),
			hist->buf, hist->cur_size, 0);
#endif

		hist->size = 0;
		hist->nbins = 0;
		hist->cur_size = 0;
		hist->cur_nbins = 0;
		return 0;
	}

#if 0 /* FIX */
	dbg_assert (hist->cur_size < hist->size);
#endif
	return 0;
}

static int
s_task_hist_init (task_t* self)
{
	dbg_assert (self != NULL);

	static struct s_task_hist_data_t hist;
	hist.discard = 1;

	self->data = &hist;
	return 0;
}

static int
s_task_hist_fin (task_t* self)
{
	dbg_assert (self != NULL);

	self->data = NULL;
	return 0;
}
