/*
 * ----------------------------------------------------------------------------
 * -------------------------------- DEV NOTES ---------------------------------
 * ----------------------------------------------------------------------------
 * There is a separate thread for each "task". Threads are zactors.
 * Currently there are two tasks:
 * 1) Listen on a REP socket and save all frames to file (until a requested
 *    number of ticks pass).
 * 2) Collate MCA frames for publishing via a PUB socket.
 *
 * Tasks have read-only access to rings (they cannot modify the cursor or head)
 * and each task keeps its own head (for each ring), which is visible by the
 * coordinator (tesd.c). For each ring, the coordinator sets the true head
 * to the per-task head which lags behind all others.
 *
 * Tasks are largely similar, so we pass the same handler, s_task_shim, to
 * zactor_new. It is responsible for doing most of the work.
 * Tasks are described by the following structure:
 * struct _task_t
 * {
 *         zloop_reader_fn* client_handler;
 *         zloop_t*         loop;
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
 *         tes_ifdesc* ifd;            // netmap interface
 *         uint32_t    heads[NUM_RINGS]; // per-ring task's head
 *         uint16_t    nrings;         // number of rings <= NUM_RINGS
 *         uint16_t    prev_fseq;      // previous frame sequence
 *         uint16_t    prev_pseq_mca;  // previous MCA protocol sequence
 *         uint16_t    prev_pseq_tr;   // previous trace protocol sequence
 *         bool        automute;       // s_task_(de)activate will enable/disable
 *                                     // client_handler
 *         bool        autoactivate;   // s_task_shim will activate task
 *         bool        just_activated; // first packet dispatch after activation
 *         bool        error;          // see below
 *         bool        busy;           // see below
 *         bool        active;         // see below
 * };
 *
 * s_task_shim registers a generic reader, s_sig_hn, for handling the signals
 * from the coordinator. Upon SIG_STOP s_sig_hn exits, upon SIG_WAKEUP it calls
 * calls the task's specific packet handler for each packet in each ring (TO
 * DO). 
 * It keeps track of the previous frame and protocol sequences (the task's
 * packet handler can make use of those as well, e.g. to track lost frames).
 * For convenience the number of missed frames (difference between previous and
 * current frame sequences mod 2^16) is passed to the pkt_handler.
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
 * Tasks defined with the autoactivate flag on are activated before entering
 * the loop. Otherwise the task should activate itself from within its
 * initializer or in its client frontend handler.
 *
 * Tasks defined with the automute flag must have a client_handler. The handler
 * will then be deregistered from the loop upon task activation, and registered
 * again upon deactivation.
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
 *
 * If either handler encounters a fatal error, it returns with TASK_ERROR.
 *
 * If the task wants to deactivate itself, it should call s_task_deactivate.
 * Alternatively it can return with TASK_SLEEP from within the pkt_handler.
 * The task then won't be receiving SIG_WAKEUP and its heads won't be
 * synchronized with the real heads.
 *
 * After talking to a client, if it needs to process packets again,
 * the task must reactivate via s_task_activate. Note that tasks which do not
 * talk to clients have no way of reactivating themselves, so their pkt_handler
 * should never return with TASK_SLEEP.
 *
 * The error, busy and active flags are handled by s_sig_hn and s_task_shim.
 * Tasks' handlers should only make use of s_task_activate, s_task_deactivate
 * and return codes (0, TASK_SLEEP or TASK_ERROR).
 *
 * Tasks are defined in a static global array, see THE TASK LIST.
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
 * ----------------------------------------------------------------------------
 * ---------------------------------- TO DO -----------------------------------
 * ----------------------------------------------------------------------------
 * - !! Check if repeating the loop over all rings until no more packets is better
 *   than exiting and waiting for a WAKEUP: no. of rings_dispatched should be
 *   > wakeups - wakeups_false if a benefit is there
 * - Set umask for the save-to-file task.
 * - Check filename for non-printable and non-ASCII characters.
 * - Why does writing to file fail with "unhandled syscall" when running under
 *   valgrind? A: https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=219715
 * - Return a string error in case of a failed request or job.
 * - Check what happens if client drops out before reply is sent (will the
 *   socket block)?
 * - Write REQ job statistics in a global database such that it can be looked
 *   up by filename, client IP or time frame.
 * - Print debugging stats every UPDATE_INTERVAL via the coordinator.
 * - FIX: why does the save-to-file count more missed packets than coordinator?
 */

#include "tesd_tasks.h"
#include "common.h"
#include <aio.h>

/* From netmap_user.h */
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#if __BYTE_ORDER == TES_BYTE_ORDER
#  define htofs
#  define htofl
#else
#  define htofs bswap16
#  define htofl bswap32
#endif

/* ---------------------------------- API ---------------------------------- */

typedef int (s_data_fn)(task_t*);
typedef int (s_pkt_fn)(zloop_t*, tespkt*, uint16_t, uint16_t, int, task_t*);

/* See DEV NOTES */
struct _task_t
{
	zloop_reader_fn* client_handler;
	zloop_t*         loop;
	s_pkt_fn*        pkt_handler;
	s_data_fn*       data_init;
	s_data_fn*       data_fin;
	void*       data;
	zactor_t*   shim;
	zsock_t*    frontend;
	const char* front_addr;
	const int   front_type;
	int         id;
	tes_ifdesc* ifd;
	uint32_t    heads[NUM_RINGS];
	uint16_t    nrings;
	uint16_t    prev_fseq;
	uint16_t    prev_pseq_mca;
	uint16_t    prev_pseq_tr;
	bool        automute;
	bool        autoactivate;
	bool        just_activated;
	bool        error;
	bool        busy;
	bool        active;
#ifdef ENABLE_FULL_DEBUG
	struct
	{
		uint64_t wakeups;
		uint64_t wakeups_inactive; /* woken up when inactive */
		uint64_t wakeups_false; /* woken up when no new packets */
		uint64_t rings_dispatched;
		struct
		{
			uint64_t rcvd_in[NUM_RINGS];
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

/* Return codes for task's socket handlers */
#define TASK_SLEEP  1
#define TASK_ERROR -1

static zloop_reader_fn s_sig_hn;
static zloop_reader_fn s_die_hn;
static zactor_fn       s_task_shim;

static int  s_task_start (tes_ifdesc* ifd, task_t* self);
static void s_task_stop (task_t* self);
static inline void s_task_activate (task_t* self);
static inline int  s_task_deactivate (task_t* self);
static int s_task_next_ring (task_t* self, uint16_t* missed_p);
static int s_task_dispatch (task_t* self, zloop_t* loop,
		uint16_t ring_id, uint16_t missed);

/* --------------------------- SAVE-TO-FILE TASK --------------------------- */

/* See README */
#define TSAVE_REQ_OK    0 // accepted
#define TSAVE_REQ_INV   1 // malformed request
#define TSAVE_REQ_ABORT 2 // no such job (for status query) or file exist (for no-overwrite)
#define TSAVE_REQ_EPERM 3 // filename is not allowed
#define TSAVE_REQ_FAIL  4 // other error opening the file, nothing was written
#define TSAVE_REQ_ERR   5 // error while writing, less than minimum requested was saved
#define TSAVE_REQ_PIC     "s881"
#define TSAVE_REP_PIC "18888888"

#define TSAVE_FIDX_LEN 16         // frame index
#define TSAVE_TIDX_LEN  8         // tick index
#define TSAVE_SIDX_LEN 16         // MCA and trace indices
#define TSAVE_STAT_LEN 64         // job statistics
#define TSAVE_ROOT "/media/data/" // must have a trailing slash

#define TSAVE_REQUIRE_FILENAME    // for now we don't generate filenames
// #define TSAVE_SINGLE_FILE         // save all payloads (with headers) to single .dat file
#ifdef TSAVE_SINGLE_FILE
#  ifndef TSAVE_SAVE_HEADERS
#    define TSAVE_SAVE_HEADERS
#  endif
#endif
// #define TSAVE_SAVE_HEADERS        // save headers in .*dat files
// #define TSAVE_NO_BAD_FRAMES       // drop bad frames

/* Employ a buffer zone for asynchronous writing. We memcpy frames into the
 * bufzone, between its head and cursor (see s_task_save_data_t below) and
 * queue batches with aio_write. aio_write has significant overhead and is not
 * worth queueing less than ~2kB (it'd be much slower than synchronous write).
 * */
#define TSAVE_BUFSIZE 10485760UL // 10 MB
#define TSAVE_MINSIZE 512000UL   // 500 kB
#ifdef ENABLE_FULL_DEBUG
#  define TSAVE_HISTBINS 11
#endif

/*
 * Transformed packet type byte: for the frame index
 */
struct s_task_save_ftype_t
{
	/* PT: */
#define TSAVE_FTYPE_PEAK        0
#define TSAVE_FTYPE_AREA        1
#define TSAVE_FTYPE_PULSE       2
#define TSAVE_FTYPE_TRACE_SGL   3
#define TSAVE_FTYPE_TRACE_AVG   4
#define TSAVE_FTYPE_TRACE_DP    5
#define TSAVE_FTYPE_TRACE_DP_TR 6
#define TSAVE_FTYPE_TICK        7
#define TSAVE_FTYPE_MCA         8
#define TSAVE_FTYPE_BAD         9
	uint8_t PT  : 4;
	uint8_t     : 3; /* reserved */
	uint8_t SEQ : 1; /* sequence error in event stream */
};
#define linear_etype(pkt_type,tr_type) \
	( (pkt_type == PKT_TYPE_TRACE) ? 3 + tr_type : pkt_type )

/*
 * Statistics sent as a reply and saved to the file. 
 */
struct s_task_save_stats_t
{
	uint64_t ticks;
	uint64_t events;         // number of events written 
	uint64_t traces;         // number of traces written 
	uint64_t hists;          // number of histograms written 
	uint64_t frames;         // total frames saved
	uint64_t frames_lost;    // total frames lost
	uint64_t frames_dropped; // total frames dropped
	uint64_t errors;         // TO DO: last 8-bytes of the tick header 
};

/*
 * Data related to a stream or index file, e.g. ticks or MCA frames.
 */
struct s_task_save_aiobuf_t
{
	struct aiocb aios;
	struct
	{
		unsigned char* base; // mmapped, size of TSAVE_BUFSIZE
		unsigned char* tail; // start address queued for aio_write
		unsigned char* cur;  // address where next packet will be coppied to
		unsigned char* ceil; // base + TSAVE_BUFSIZE
		size_t waiting;      // copied into buffer since the last aio_write
		size_t enqueued;     // queued for writing at the last aio_write
#ifdef ENABLE_FULL_DEBUG
		struct {
			size_t prev_enqueued;
			size_t prev_waiting;
			size_t last_written;
			uint64_t batches[TSAVE_HISTBINS];
			uint64_t failed_batches;
			uint64_t num_skipped;
			uint64_t num_blocked;
		} st;
#endif
	} bufzone;
	size_t size; // number of bytes written
};

/*
 * The frame index.
 * Flags mca, bad and seq are set in event type.
 */
struct s_task_save_fidx_t
{
	uint64_t start;   // frame's offset into its corresponding dat file
	uint32_t length;  // payload's length
	uint16_t esize;   // original event size
	uint8_t  changed; // event frame differs from previous
	struct s_task_save_ftype_t ftype; // see definition of struct
};

/*
 * The tick index.
 */
struct s_task_save_tidx_t
{
	uint32_t start_frame; // frame number of first non-tick event
	uint32_t stop_frame;  // frame number of last non-tick event
};

/*
 * The MCA and trace indices. (the 's' is for 'stream')
 */
struct s_task_save_sidx_t
{
	uint64_t start;  // first byte of histogram/trace into dat file
	uint64_t length; // length in bytes of histogram/trace
};

/*
 * Data for the currently-saved file. min_ticks and filename are set when
 * receiving a request from client.
 */
struct s_task_save_data_t
{
	struct s_task_save_stats_t st;

	struct {
#ifdef TSAVE_SINGLE_FILE
		struct s_task_save_aiobuf_t dat;  // all payloads
#else
		struct s_task_save_aiobuf_t bdat; // bad payloads
		struct s_task_save_aiobuf_t mdat; // MCA payloads
		struct s_task_save_aiobuf_t tdat; // tick payloads
		struct s_task_save_aiobuf_t edat; // event payloads
#endif
		struct s_task_save_aiobuf_t fidx; // frame index
		struct s_task_save_aiobuf_t midx; // MCA index
		struct s_task_save_aiobuf_t tidx; // tick index
		struct s_task_save_aiobuf_t ridx; // trace index
	} aio;

	struct
	{
		struct s_task_save_sidx_t idx;
		size_t size;
		size_t cur_size;
		bool is_event; // i.e. is_trace, otherwise it's MCA
		bool discard;  // stream had errors, ignore rest
	} cur_stream;          // ongoing trace of histogram

	struct
	{
		struct s_task_save_tidx_t idx;
		uint32_t nframes; // no. of event frames in this tick
	} cur_tick;
	uint8_t  prev_esize; // event size for previous event
	uint8_t  prev_etype; // transformed event type for previous event, see PT above

	uint64_t min_ticks;
	uint64_t min_events;
#ifdef ENABLE_FULL_DEBUG
	unsigned char prev_hdr[TES_HDR_LEN];
#endif
	char*    filename;  // filename statistics are written to
	int      fd;        // fd for the above file
	bool     recording; // wait for a tick before starting capture
};

/* Handlers */
static zloop_reader_fn s_task_save_req_hn;
static s_pkt_fn        s_task_save_pkt_hn;

/* Task initializer and finalizer */
static s_data_fn       s_task_save_init;
static s_data_fn       s_task_save_fin;
static int   s_task_save_init_aiobuf (struct s_task_save_aiobuf_t* aiobuf);
static void  s_task_save_fin_aiobuf (struct s_task_save_aiobuf_t* aiobuf);

/*
 * s_task_save_open and s_task_save_close deal with stream and index files
 * only. stats_* deal with the stats file/database.
 * s_task_save_req_hn will read in filename, min_ticks and min_events (see
 * s_task_save_data_t) and s_task_save_stats_send will reset those, so should
 * be called at the very end (after processing is done and reply is sent to the
 * client).
 */
/* Job initializer and finalizer */
static int   s_task_save_open  (struct s_task_save_data_t* sjob, mode_t fmode);
static void  s_task_save_close (struct s_task_save_data_t* sjob);
static int   s_task_save_open_aiobuf (struct s_task_save_aiobuf_t* aiobuf,
	const char* filename, mode_t fmode);
static void  s_task_save_close_aiobuf (struct s_task_save_aiobuf_t* aiobuf);

/* Statistics for a job. */
static int   s_task_save_stats_read  (struct s_task_save_data_t* sjob);
static int   s_task_save_stats_write (struct s_task_save_data_t* sjob);
static int   s_task_save_stats_send  (struct s_task_save_data_t* sjob,
	zsock_t* frontend);

/* Ongoing job helpers */
static void  s_task_save_flush (struct s_task_save_data_t* sjob);
static int   s_task_save_try_queue_aiobuf (struct s_task_save_aiobuf_t* aiobuf,
	const char* buf, uint16_t len, int task_id);
static int   s_task_save_queue_aiobuf (struct s_task_save_aiobuf_t* aiobuf,
	bool force);
static char* s_task_save_canonicalize_path (const char* filename,
	bool checkonly, int task_id);

#ifdef ENABLE_FULL_DEBUG
static void  s_task_save_dbg_aiobuf_stats (struct s_task_save_aiobuf_t* aiobuf,
	const char* stream, int task_id);
#endif

/* --------------------------- PUBLISH HIST TASK --------------------------- */

#ifndef TES_MCASIZE_BUG
#define THIST_MAXSIZE 65528U // highest 16-bit number that is a multiple of 8 bytes
#else
#define THIST_MAXSIZE 65576U
#endif

/*
 * Data for currently built histogram.
 */
struct s_task_hist_data_t
{
#ifdef ENABLE_FULL_DEBUG
	uint64_t      published; // number of published histograms
	uint64_t      dropped;   // number of aborted histograms
#endif
	uint16_t      nbins;     // total number of bins in histogram
	uint16_t      cur_nbins; // number of received bins so far
#ifndef TES_MCASIZE_BUG
	uint16_t      size;      // size of histogram including header
	uint16_t      cur_size;  // number of received bytes so far
#else
	uint32_t      size;      // size of histogram including header
	uint32_t      cur_size;  // number of received bytes so far
#endif
	bool          discard;   // discard all frames until the next header
	unsigned char buf[THIST_MAXSIZE];
};

/* There is no handler for the PUB socket. */
static s_pkt_fn        s_task_hist_pkt_hn;

/* Task initializer and finalizer */
static s_data_fn       s_task_hist_init;
static s_data_fn       s_task_hist_fin;

/* ------------------------- GET AVERAGE TRACE TASK ------------------------ */

#define TAVGTR_REQ_OK    0 // accepted
#define TAVGTR_REQ_INV   1 // malformed request
#define TAVGTR_REQ_TOUT  2 // timeout
#define TAVGTR_REQ_ERR   3 // dropped trace
#define TAVGTR_REQ_PIC       "4"
#define TAVGTR_REP_PIC_OK   "1b"
#define TAVGTR_REP_PIC_FAIL  "1"
#define TAVGTR_MAXSIZE 65528U // highest 16-bit number that is a multiple of 8 bytes

/*
 * Data for currently built average trace.
 */
struct s_task_avgtr_data_t
{
	int           timer;     // returned by zloop_timer
	uint16_t      size;      // size of histogram including header
	uint16_t      cur_size;  // number of received bytes so far
	bool          recording; // discard all frames until the first header
	unsigned char buf[TAVGTR_MAXSIZE];
};

/* Handlers */
static zloop_reader_fn s_task_avgtr_req_hn;
static zloop_timer_fn  s_task_avgtr_timeout_hn;
static s_pkt_fn        s_task_avgtr_pkt_hn;

/* Task initializer and finalizer */
static s_data_fn       s_task_avgtr_init;
static s_data_fn       s_task_avgtr_fin;

/* ----------------------------- THE TASK LIST ----------------------------- */

#define NUM_TASKS 3
static task_t s_tasks[] = {
	{ // SAVE TO FILE 
		.client_handler = s_task_save_req_hn,
		.pkt_handler    = s_task_save_pkt_hn,
		.data_init      = s_task_save_init,
		.data_fin       = s_task_save_fin,
		.front_addr     = "tcp://*:55555",
		.front_type     = ZMQ_REP,
		.automute       = 1,
	},
	{ // GET AVG TRACE
		.client_handler = s_task_avgtr_req_hn,
		.pkt_handler    = s_task_avgtr_pkt_hn,
		.data_init      = s_task_avgtr_init,
		.data_fin       = s_task_avgtr_fin,
		.front_addr     = "tcp://*:55556",
		.front_type     = ZMQ_REP,
		.automute       = 1,
	},
	{ // PUBLISH HIST
		.pkt_handler    = s_task_hist_pkt_hn,
		.data_init      = s_task_hist_init,
		.data_fin       = s_task_hist_fin,
		.front_addr     = "tcp://*:55565",
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
tasks_start (tes_ifdesc* ifd, zloop_t* c_loop)
{
	assert (ifd != NULL);
	assert (NUM_TASKS == sizeof (s_tasks) / sizeof (task_t));
	int rc;
	for (int t = 0; t < NUM_TASKS; t++)
	{
		s_tasks[t].id = t + 1;
		s_msg (0, LOG_DEBUG, 0, "Starting task #%d", t + 1);
		rc = s_task_start (ifd, &s_tasks[t]);
		if (rc != 0)
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
	assert (loop != NULL);
	int rc;
	for (int t = 0; t < NUM_TASKS; t++)
	{
		s_msg (0, LOG_DEBUG, 0, "Registering reader for task #%d", t);
		task_t* self = &s_tasks[t];
		rc = zloop_reader (loop, zactor_sock(self->shim),
			s_die_hn, NULL);
		if (rc != 0)
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
	assert (loop != NULL);
	for (int t = 0; t < NUM_TASKS; t++)
	{
		s_msg (0, LOG_DEBUG, 0,
			"Unregistering reader for task #%d", t);
		task_t* self = &s_tasks[t];
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
		task_t* self = &s_tasks[t];
		if (self->active && ! self->busy)
		{
			int rc = zsock_signal (self->shim, SIG_WAKEUP);
			if (rc != 0)
			{
				s_msg (errno, LOG_ERR, 0,
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
		s_msg (0, LOG_DEBUG, 0, "Stopping task #%d", t);
		task_t* self = &s_tasks[t];
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

	bool updated = 0; /* set to 1 if at least one active task */
	for (int t = 0; t < NUM_TASKS; t++)
	{
		task_t* self = &s_tasks[t];
		if (self->active)
		{
			/* The first time an active task is found, take its
			 * head, for each following active task, compare its
			 * head with the currently slowest one. */
			if (updated)
			{
				for (int r = 0; r < NUM_RINGS; r++)
				{
					tes_ifring* rxring = tes_if_rxring (self->ifd, r);
					heads[r] = tes_ifring_earlier_id (
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
	
#ifdef ENABLE_FULL_DEBUG
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

	/* We should never have received a WAKEUP if we are not active, but we
	 * do, after a job is done. Signals must be queueing in the PAIR socket
	 * and coming with a slight delay. */
	if ( ! self->active )
	{
#ifdef ENABLE_FULL_DEBUG
		if (self->dbg_stats.wakeups_inactive == 0)
			s_msg (0, LOG_DEBUG, self->id,
				"First inactive wakeup");
		self->dbg_stats.wakeups_inactive++;
#endif
		return 0;
	}
#ifdef ENABLE_FULL_DEBUG
	self->dbg_stats.wakeups++;
#endif
	self->busy = 1;

	/* Process packets. */
#ifdef ENABLE_FULL_DEBUG
	bool first = 1;
#endif
	while (1)
	{
		uint16_t missed;   /* will hold the jump in frame seq */
		int next_ring_id = s_task_next_ring (self, &missed);

		/*
		 * We should never have received a WAKEUP if there are no new
		 * packets, but sometimes we do, why?
		 */
		if (next_ring_id < 0)
		{
#ifdef ENABLE_FULL_DEBUG
			if (first)
				self->dbg_stats.wakeups_false++;
#endif
			break;
		}
#ifdef ENABLE_FULL_DEBUG
		first = 0;
#endif

		int rc = s_task_dispatch (self, loop, next_ring_id, missed);
		/* In case packet hanlder or dispatcher need to know that it's the
		 * first time after activation. */
		self->just_activated = 0;

		if (rc == TASK_SLEEP)
		{
			rc = s_task_deactivate (self); /* will return TASK_ERROR on error */
			break;
		}

		if (rc == TASK_ERROR)
		{ /* either pkt_handler or s_task_deactivate failed */
			self->error = 1;
			break;
		}
	}

	self->busy = 0;
	return (self->error ? -1 : 0);
}

/*
 * Registered with the coordinator's loop. Receives SIG_DIED sent by a task and
 * terminates the coordinator's loop. 
 */
static int
s_die_hn (zloop_t* loop, zsock_t* reader, void* ignored)
{
	dbg_assert (ignored == NULL);

#ifdef ENABLE_FULL_DEBUG
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
	assert (self_ != NULL);
	zsock_signal (pipe, 0); /* zactor_new will wait for this */

	int rc;
	task_t* self = (task_t*) self_;
	assert (self->pkt_handler != NULL);
	assert (self->ifd != NULL);
	assert (self->id > 0);
	
	zloop_t* loop = zloop_new ();
	self->loop = loop;
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
		if (rc != 0)
		{
			s_msg (errno, LOG_ERR, self->id,
				"Could not bind the public interface");
			self->error = 1;
			goto cleanup;
		}
		s_msg (0, LOG_INFO, self->id,
			"Listening on port(s) %s", self->front_addr);
	}
	/* Register the readers */
	if (self->automute)
		assert (self->frontend != NULL &&
				self->client_handler != NULL);

	rc = zloop_reader (loop, pipe, s_sig_hn, self);
	if (self->client_handler != NULL)
	{
		assert (self->frontend != NULL);
		rc |= zloop_reader (loop, self->frontend,
				self->client_handler, self);
	}
	if (rc != 0)
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
		if (rc != 0)
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
		if (rc != 0)
		{
			s_msg (errno, LOG_ERR, self->id,
				"Could not cleanup thread data");
		}
		dbg_assert (self->data == NULL);
	}
	zloop_destroy (&loop);
	zsock_destroy (&self->frontend);
	s_msg (0, LOG_DEBUG, self->id, "Done");
#ifdef ENABLE_FULL_DEBUG
	s_msg (0, LOG_DEBUG, self->id,
		"Woken up %lu times, %lu when not active, "
		"%lu when no new packets, dispatched "
		"%lu rings, %lu packets missed",
		self->dbg_stats.wakeups,
		self->dbg_stats.wakeups_inactive,
		self->dbg_stats.wakeups_false,
		self->dbg_stats.rings_dispatched,
		self->dbg_stats.pkts.missed
		);
	for (int r = 0; r < NUM_RINGS; r++)
		s_msg (0, LOG_DEBUG, self->id,
			"Ring %d received: %lu", r, 
			self->dbg_stats.pkts.rcvd_in[r]);
#endif
}

/*
 * Initializes a task_t and starts a new thread using zactor_new.
 * Registers the task's back end of the pipe with the coordinator's loop.
 * Returns 0 on success, -1 on error.
 */
static int
s_task_start (tes_ifdesc* ifd, task_t* self)
{
	assert (self != NULL);
	assert (ifd != NULL);

	self->ifd = ifd;
	assert (tes_if_rxrings (ifd) == NUM_RINGS);

	/* Start the thread, will block until the handler signals */
	self->shim = zactor_new (s_task_shim, self);
	assert (self->shim != NULL);
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
	assert (rc == SIG_INIT);
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
	assert (self != NULL);
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
 * If the task handles one client at a time, disables reading the
 * client_handler.
 */
static inline void
s_task_activate (task_t* self)
{
	assert (self != NULL);

	if (self->automute)
		zloop_reader_end (self->loop, self->frontend);

	for (int r = 0; r < NUM_RINGS; r++)
	{
		tes_ifring* rxring = tes_if_rxring (self->ifd, r);
		self->heads[r] = tes_ifring_head (rxring);
	}
	self->active = 1;
	self->just_activated = 1;
}

/*
 * Deactivates the task and, if the task handles one client at a time, enables
 * reading the client_handler.
 * Returns 0 on success, TASK_ERROR on error.
 */
static inline int
s_task_deactivate (task_t* self)
{
	if (self->automute)
	{
		int rc = zloop_reader (self->loop, self->frontend,
				self->client_handler, self);
		if (rc == -1)
		{
			s_msg (errno, LOG_ERR, self->id,
				"Could not re-enable the zloop reader");
			return TASK_ERROR;
		}
	}

	self->active = 0;

	return 0;
}

/*
 * Chooses the ring which contains the next packet to be inspected.
 */
static int s_task_next_ring (task_t* self, uint16_t* missed_p)
{
	dbg_assert (self != NULL);

	int next_ring_id = -1; /* next to process */

	if (unlikely (self->just_activated))
	{
		/*
		 * If first time after activation, set the previous sequence
		 * and choose the ring by comparing the heads of all rings.
		 * Find the "smallest" frame sequence among the heads. Treat
		 * seq. no. A as after seq. no.  B if B - A is > UINT16_MAX/2.
		 */
		uint16_t thres_gap = (uint16_t)~0 >> 1;
		for (int r = 0; r < NUM_RINGS; r++)
		{
			tes_ifring* rxring = tes_if_rxring (self->ifd, r);
			if (tes_ifring_tail (rxring) == self->heads[r])
				continue;
			tespkt* pkt = (tespkt*) tes_ifring_buf (
					rxring, self->heads[r]);
			uint16_t cur_fseq = tespkt_fseq (pkt);
			if (r == 0 || cur_fseq - self->prev_fseq > thres_gap)
			{
				self->prev_fseq = cur_fseq - 1;
				next_ring_id = r;
			}
		}
		if (missed_p != NULL)
			*missed_p = 0;
	}
	else
	{
		/*
		 * Otherwise, choose the ring based on prev_seq. Allowing for
		 * lost frames, simply take the ring for which the task's head
		 * packet is closest in sequence to the last seen frame
		 * sequence.
		 */
		uint16_t missed = ~0;  /* will hold the jump in frame seq,
					* initialize to UINT16_MAX */
		for (int r = 0; r < NUM_RINGS; r++)
		{
			tes_ifring* rxring = tes_if_rxring (self->ifd, r);
			if (tes_ifring_tail (rxring) == self->heads[r])
				continue;
			tespkt* pkt = (tespkt*) tes_ifring_buf (
					rxring, self->heads[r]);
			uint16_t cur_fseq = tespkt_fseq (pkt);
			uint16_t fseq_gap = cur_fseq - self->prev_fseq - 1;
			if (fseq_gap <= missed)
			{
				next_ring_id = r;
				missed = fseq_gap;
				if (fseq_gap == 0)
					break;
			}
		}
		if (missed_p != NULL)
			*missed_p = missed;
	}

	return next_ring_id;
}

/*
 * Loops over the given ring until either reaching the tail or seeing
 * a discontinuity in frame sequence. For each buffer calls the task's
 * pkt_handler.
 * Returns 0 if all packets until the tail are processed.
 * Returns TASK_SLEEP or TASK_ERR if pkt_handler does so.
 * Returns ?? if a jump in frame sequence is seen (TO DO).
 */

static int s_task_dispatch (task_t* self, zloop_t* loop,
		uint16_t ring_id, uint16_t missed)
{
	dbg_assert (self != NULL);
	dbg_assert (loop != NULL);

	tes_ifring* rxring = tes_if_rxring (self->ifd, ring_id);
	dbg_assert ( self->heads[ring_id] != tes_ifring_tail (rxring) );
#ifdef ENABLE_FULL_DEBUG
	self->dbg_stats.rings_dispatched++;
#if 0
	if (missed)
	{
		tespkt* pkt = (tespkt*) tes_ifring_buf (
				rxring, self->heads[ring_id]);
		s_msg (0, LOG_DEBUG, self->id,
				"Dispatching ring %hu: missed %hu at frame %hu",
				ring_id, missed, tespkt_fseq (pkt));
	}
#endif
#endif

	/*
	 * First exec of the loop uses the head from the last time
	 * dispatch was called with this ring_id.
	 */
	uint16_t fseq_gap = 0;
#ifdef ENABLE_FULL_DEBUG
	bool first = 1;
#endif
	for ( ; self->heads[ring_id] != tes_ifring_tail (rxring);
		self->heads[ring_id] = tes_ifring_following (
				rxring, self->heads[ring_id]) )
	{
		/* FIX: TO DO: return code for a jump in fseq */
		// if (fseq_gap > 0)
		//         return 0;

		tespkt* pkt = (tespkt*) tes_ifring_buf (
			rxring, self->heads[ring_id]);
		dbg_assert (pkt != NULL);

		/*
		 * Check packet.
		 */
		int err = tespkt_is_valid (pkt);
#ifdef ENABLE_FULL_DEBUG
		if (err != 0)
		{
			s_msg (0, LOG_DEBUG, self->id,
				"Packet invalid, error is 0x%x", err);
		}
#endif
		uint16_t len = tes_ifring_len (rxring, self->heads[ring_id]);
		uint16_t flen = tespkt_flen (pkt);
		if (flen > len)
		{
#ifdef ENABLE_FULL_DEBUG
			s_msg (0, LOG_DEBUG, self->id,
				"Packet too long (header says %hu, "
				"ring slot is %hu)", flen, len);
#endif
			err |= TES_EETHLEN;
			flen = len;
		}
		dbg_assert (flen <= MAX_TES_FRAME_LEN);

		uint16_t cur_fseq = tespkt_fseq (pkt);
		fseq_gap = cur_fseq - self->prev_fseq - 1;
#ifdef ENABLE_FULL_DEBUG
		if (first)
			dbg_assert (fseq_gap == missed);
		first = 0;
		self->dbg_stats.pkts.rcvd_in[ring_id]++;
		self->dbg_stats.pkts.missed += fseq_gap;
#endif

		int rc = self->pkt_handler (loop, pkt, flen, fseq_gap, err, self);

		self->prev_fseq = cur_fseq;
		if (tespkt_is_mca (pkt))
			self->prev_pseq_mca = tespkt_pseq (pkt);
		else if (tespkt_is_trace (pkt) &&
				! tespkt_is_trace_dp (pkt))
			self->prev_pseq_tr = tespkt_pseq (pkt);

		if (rc != 0)
			return rc; /* pkt_handler doesn't want more */
	}

	return 0;
}

/* ------------------------------------------------------------------------- */
/* --------------------------- SAVE-TO-FILE TASK --------------------------- */
/* ------------------------------------------------------------------------- */

/*
 * Called when a client sends a request on the REP socket. For valid requests
 * of status, opens the file and send the reply. For valid requests to save,
 * opens the file and marks the task as active.
 */
static int
s_task_save_req_hn (zloop_t* loop, zsock_t* reader, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;

	struct s_task_save_data_t* sjob =
		(struct s_task_save_data_t*) self->data;
	dbg_assert ( ! sjob->recording );

	uint8_t job_mode;

	char* filename;
	int rc = zsock_recv (reader, TSAVE_REQ_PIC, &filename,
		&sjob->min_ticks, &sjob->min_events, &job_mode);
	if (rc == -1)
	{ /* would also return -1 if picture contained a pointer (p) or a null
	   * frame (z) but message received did not match this signature; this
	   * is irrelevant in this case */
		s_msg (0, LOG_DEBUG, self->id, "Receive interrupted");
		return TASK_ERROR;
	}

	/* Is the request understood? */
	if (filename == NULL || job_mode > 1)
	{
		s_msg (0, LOG_INFO, self->id,
			"Received a malformed request");
		zsock_send (reader, TSAVE_REP_PIC, TSAVE_REQ_INV,
				0, 0, 0, 0, 0, 0, 0);
		return 0;
	}

	/* Is it only a status query? */
	bool checkonly = (sjob->min_ticks == 0);
	if (checkonly)
	{
		s_msg (0, LOG_INFO, self->id,
			"Received request for status of '%s'",
			filename);
	}
	else
	{
		s_msg (0, LOG_INFO, self->id,
			"Received request to write %lu ticks and "
			"%lu events to '%s'",
			sjob->min_ticks, sjob->min_events, filename);
	}

	/* Check if filename is allowed and get the realpath. */
	sjob->filename = s_task_save_canonicalize_path (
			filename, checkonly, self->id);
	zstr_free (&filename); /* nullifies the pointer */

	if (sjob->filename == NULL)
	{
		if (checkonly)
		{
			s_msg (0, LOG_INFO, self->id,
					"Job not found");
			zsock_send (reader, TSAVE_REP_PIC, TSAVE_REQ_ABORT,
					0, 0, 0, 0, 0, 0, 0);
		}
		else
		{
			s_msg (errno, LOG_INFO, self->id,
					"Filename is not valid");
			zsock_send (reader, TSAVE_REP_PIC, TSAVE_REQ_EPERM,
					0, 0, 0, 0, 0, 0, 0);
		}

		return 0;
	}

	dbg_assert (sjob->filename != NULL);

	/*
	 * *****************************************************************
	 * ************************** Status query. ************************
	 * *****************************************************************
	 */
	if (checkonly)
	{ /* just read in stats and send reply */
		rc = s_task_save_stats_read  (sjob);
		if (rc != 0)
		{
			s_msg (errno, LOG_ERR, self->id,
				"Could not read stats");
			zsock_send (reader, TSAVE_REP_PIC, TSAVE_REQ_FAIL,
					0, 0, 0, 0, 0, 0, 0);
			return 0;
		}
		rc = s_task_save_stats_send  (sjob, self->frontend);
		if (rc != 0)
		{
			s_msg (0, LOG_NOTICE, self->id,
				"Could not send stats");
		}
		return 0;
	}

	/*
	 * *****************************************************************
	 * ************************* Write request. ************************
	 * *****************************************************************
	 */
	/*
	 * Set the file open mode and act according to the return status of
	 * open and errno (print a warning of errno is unexpected)
	 * Request is for:
	 *   create: create if non-existing
	 *           - if successful, enable save
	 *           - if failed, send reply (expect errno == EEXIST)
	 *   create: create or overwrite
	 *           - if successful, enable save
	 *           - if failed, send reply (this shouldn't happen)
	 */
	int exp_errno = 0;
	mode_t fmode = O_RDWR | O_CREAT;
	if (job_mode == 0)
	{ /* do not overwrite */
		fmode |= O_EXCL;
		exp_errno = EEXIST;
	}

	rc = s_task_save_open (sjob, fmode);
	if (rc == -1)
	{
		if (errno != exp_errno)
		{
			s_msg (errno, LOG_ERR, self->id,
				"Could not open file %s",
				sjob->filename);
			zsock_send (reader, TSAVE_REP_PIC, TSAVE_REQ_FAIL,
					0, 0, 0, 0, 0, 0, 0);
		}
		else
		{
			s_msg (0, LOG_INFO, self->id, "Job will not proceed");
			zsock_send (reader, TSAVE_REP_PIC, TSAVE_REQ_ABORT,
					0, 0, 0, 0, 0, 0, 0);
		}
		s_task_save_close (sjob);
		return 0;
	}

	s_msg (0, LOG_INFO, self->id,
		"Opened files %s.* for writing", sjob->filename);

	/* Disable polling on the reader until the job is done. Wakeup packet
	 * handler. */
	s_task_activate (self);

	return 0;
}

/*
 * Saves packet payloads to corresponding file(s) and writes index files.
 */
static int
s_task_save_pkt_hn (zloop_t* loop, tespkt* pkt, uint16_t flen,
		uint16_t missed, int err, task_t* self)
{
	dbg_assert (self != NULL);

	struct s_task_save_data_t* sjob =
		(struct s_task_save_data_t*) self->data;

	bool is_tick = tespkt_is_tick (pkt);
	if ( ! sjob->recording && is_tick )
		sjob->recording = 1; /* start the capture */

	if ( ! sjob->recording )
		return 0;

	if (err)
	{
#ifdef TSAVE_NO_BAD_FRAMES
		/* drop bad frames */
		sjob->st.frames_dropped++;
		return 0;
#endif
	}

	sjob->st.frames++;
	sjob->st.frames_lost += missed;

	uint16_t esize = tespkt_esize (pkt);
	esize = htofs (esize); /* in FPGA byte-order */
	uint16_t paylen = flen - TES_HDR_LEN;

	bool is_header = tespkt_is_header (pkt);
	bool is_mca = tespkt_is_mca (pkt);
	bool is_trace = ( tespkt_is_trace (pkt) &&
			! tespkt_is_trace_dp (pkt) );

	/* **** Update tick and frame indices and choose the data file. **** */
	struct s_task_save_aiobuf_t* aiofidx = &sjob->aio.fidx;
#ifdef TSAVE_SINGLE_FILE
	struct s_task_save_aiobuf_t* aiodat = &sjob->aio.dat;
#else
	struct s_task_save_aiobuf_t* aiodat = NULL; /* set later */
#endif
	struct s_task_save_fidx_t fidx;
	fidx.length = paylen;
	fidx.esize = esize;
	fidx.changed = 0;
	fidx.ftype.SEQ = 0;

	bool finishing = 0;
	int jobrc;

	/* Check for sequence error. */
	if (missed > 0)
	{
#ifdef ENABLE_FULL_DEBUG
#if 0
		s_msg (0, LOG_DEBUG, self->id,
			"Missed %hu at frame #%lu",
			missed, sjob->st.frames - 1);
#endif
#endif
		fidx.ftype.SEQ = 1;
	}

	/* Check packet type. */
	if (err)
	{
		fidx.ftype.PT = TSAVE_FTYPE_BAD;
#ifndef TSAVE_SINGLE_FILE
		aiodat = &sjob->aio.bdat;
#endif
	}
	else if (is_mca)
	{
		fidx.ftype.PT = TSAVE_FTYPE_MCA;
#ifndef TSAVE_SINGLE_FILE
		aiodat = &sjob->aio.mdat;
#endif
	}
	else if (is_tick)
	{
		fidx.ftype.PT = TSAVE_FTYPE_TICK;
#ifndef TSAVE_SINGLE_FILE
		aiodat = &sjob->aio.tdat;
#endif

		if (sjob->st.ticks > 0)
		{
			struct s_task_save_tidx_t* tidx = &sjob->cur_tick.idx;
			jobrc = s_task_save_try_queue_aiobuf (&sjob->aio.tidx, (char*)tidx,
					TSAVE_TIDX_LEN, self->id);
			if (jobrc < 0)
				finishing = 1; /* error */
		}

		sjob->cur_tick.nframes = 0;
		/* no need to zero the index */
	}
	else
	{
#ifndef TSAVE_SINGLE_FILE
		aiodat = &sjob->aio.edat;
#endif

		struct s_task_save_tidx_t* tidx = &sjob->cur_tick.idx;
		const struct tespkt_event_type* etype = tespkt_etype (pkt);
		uint8_t pt = linear_etype (etype->PKT, etype->TR);
		fidx.ftype.PT = pt;
		if ( sjob->st.frames > 1 && 
			( sjob->prev_etype != pt || sjob->prev_esize != esize ) )
		{
			fidx.changed = 1;
		}
		sjob->prev_esize = esize;
		sjob->prev_etype = pt;

		if (sjob->cur_tick.nframes == 0)
		{ /* first non-tick event frame after a tick */
			tidx->start_frame = sjob->st.frames - 1;
		}
		else
		{ /* in case it's the last event before a tick */
			tidx->stop_frame = sjob->st.frames - 1;
		}
		sjob->cur_tick.nframes++;
	}

	fidx.start = aiodat->size +
		aiodat->bufzone.waiting + aiodat->bufzone.enqueued;

	/*
	 * *************** Update statistics and stream index. *************
	 * Check if there is an ongoing stream (trace or MCA). If so, update
	 * index if necessary. If this is the last frame of a stream, queue
	 * the index for writing and reset cur_stream's size and cur_size.
	 * size and cur_size would also be reset if an error (e.g. missed
	 * frames) occurs. idx and is_event are set when receiving the header
	 * of a new stream.
	 */

	/* Skip if frame is bad */
	if (err)
		goto done;

	if (sjob->cur_stream.size > 0)
	{
		dbg_assert (sjob->cur_stream.cur_size > 0);
		dbg_assert (sjob->cur_stream.cur_size < sjob->cur_stream.size);
		dbg_assert ( ! sjob->cur_stream.discard );

	}
	else
		dbg_assert (sjob->cur_stream.cur_size == 0);

	bool continues_stream = ( ( (is_trace && sjob->cur_stream.is_event) ||
				    (is_mca && ! sjob->cur_stream.is_event) ) &&
			sjob->cur_stream.size > 0 && ! is_header && missed == 0 );
	bool starts_stream = ( (is_trace || is_mca) && is_header &&
			sjob->cur_stream.size == 0 );
	bool interrupts_stream = ( ! continues_stream &&
			sjob->cur_stream.size > 0 );

	if (interrupts_stream) 
	{ /* unexpected or first missed frames during an ongoing stream */
		sjob->cur_stream.discard = 1;
		sjob->cur_stream.size = 0;
		sjob->cur_stream.cur_size = 0;

		dbg_assert ( is_header || missed > 0 ||
			(is_trace && ! sjob->cur_stream.is_event) ||
			(is_mca && sjob->cur_stream.is_event) ||
			( ! is_trace && ! is_mca ) );
#if 0
		if (missed == 0)
		{ /* should only happen in case of FPGA fault */
			s_msg (0, LOG_NOTICE, self->id,
				"Received a%s %sframe (#%lu) "
				"while a %s was ongoing",
				is_mca ? " histogram" : (
					is_trace ? " trace" : (
						is_tick ? " tick" :
						"n event" ) ),
				is_header ? "header " : "",
				sjob->st.frames - 1,
				sjob->cur_stream.is_event ?
				"trace" : "histogram");
		}
#endif
	}

	if (starts_stream || continues_stream)
	{
		if (starts_stream)
		{ /* start a new stream */
			if (is_trace)
			{
				sjob->cur_stream.size = tespkt_trace_size (pkt);
				sjob->cur_stream.is_event = 1;
			}
			else
			{
				sjob->cur_stream.size = tespkt_mca_size (pkt);
				sjob->cur_stream.is_event = 0;
			}
			sjob->cur_stream.discard = 0;

			sjob->cur_stream.idx.start = aiodat->size +
				aiodat->bufzone.waiting + aiodat->bufzone.enqueued;
		}
		else
		{ /* ongoing multi-frame stream */
			dbg_assert ( ! sjob->cur_stream.discard && missed == 0);
		}

		sjob->cur_stream.cur_size += paylen;
		if (sjob->cur_stream.cur_size > sjob->cur_stream.size)
		{ /* extra bytes */
#ifdef ENABLE_FULL_DEBUG
#if 0
			s_msg (0, LOG_DEBUG, self->id, "Extra %s data "
					"at frame #%lu",
					is_mca ? "histogram" : "trace",
					sjob->st.frames - 1);
#endif
#endif
			sjob->cur_stream.size = 0;
			sjob->cur_stream.cur_size = 0;
			sjob->cur_stream.discard = 1;
		}
		else if (sjob->cur_stream.cur_size == sjob->cur_stream.size)
		{ /* done, record the event */
			struct s_task_save_aiobuf_t* aiosidx = NULL;
			if (is_trace)
			{
				aiosidx = &sjob->aio.ridx;
				sjob->st.events++;
				sjob->st.traces++;
			}
			else
			{
				aiosidx = &sjob->aio.midx;
				sjob->st.hists++;
			}
			sjob->cur_stream.idx.length = sjob->cur_stream.size;
			sjob->cur_stream.size = 0;
			sjob->cur_stream.cur_size = 0;

			jobrc = s_task_save_try_queue_aiobuf (aiosidx,
					(char*)&sjob->cur_stream.idx,
					TSAVE_SIDX_LEN, self->id);
			if (jobrc < 0)
				finishing = 1; /* error */
		}
	}
	else if (is_mca || is_trace)
	{ /* missed beginning of a stream or in the process of discarding */
		if ( ! interrupts_stream )
		{
			dbg_assert ( ! is_header );
			dbg_assert (sjob->cur_stream.size == 0);

			if ( ! sjob->cur_stream.discard )
			{
#ifdef ENABLE_FULL_DEBUG
#if 0
				s_msg (0, LOG_DEBUG, self->id,
					"Received a non-header %s frame (#%lu) "
					"while no stream was ongoing",
					is_mca ? "histogram" : "trace",
					sjob->st.frames - 1);
#endif
#endif
				sjob->cur_stream.discard = 1;
			}
		}
	}
	else if (is_tick)
	{ /* tick */
		sjob->st.ticks++;
		/* Ticks should be > min_ticks cause we count the
		 * starting one too. */
		if (sjob->st.ticks > sjob->min_ticks &&
				sjob->st.events >= sjob->min_events)
			finishing = 1; /* DONE */
	}
	else
	{ /* short event */
		sjob->st.events += tespkt_event_nums (pkt);
	}

done:
	/* ********************** Write frame payload. ********************* */
#ifdef TSAVE_SAVE_HEADERS
	jobrc = s_task_save_try_queue_aiobuf (aiodat, (char*)pkt,
			flen, self->id);
#else
	jobrc = s_task_save_try_queue_aiobuf (aiodat, (char*)pkt + TES_HDR_LEN,
	                 paylen, self->id);
#endif
	if (jobrc < 0)
		finishing = 1; /* error */

	/* *********************** Write frame index. ********************** */

	jobrc = s_task_save_try_queue_aiobuf (aiofidx, (char*)&fidx,
			TSAVE_FIDX_LEN, self->id);
	if (jobrc < 0)
		finishing = 1; /* error */

	dbg_assert ( sjob->st.frames * TSAVE_FIDX_LEN ==
			aiofidx->size +
			aiofidx->bufzone.waiting +
			aiofidx->bufzone.enqueued );

	/* ********************** Check if done. *************************** */
	if (finishing)
	{
		/* Flush all buffers. */
		s_task_save_flush (sjob);

		s_msg (0, LOG_INFO, self->id,
			"Finished writing %lu ticks and %lu events",
			sjob->st.ticks, sjob->st.events);
#ifdef ENABLE_FULL_DEBUG
#ifdef TSAVE_SINGLE_FILE
		s_task_save_dbg_aiobuf_stats (&sjob->aio.dat, "ALL data", self->id);
#else
		s_task_save_dbg_aiobuf_stats (&sjob->aio.bdat, "BAD data", self->id);
		s_task_save_dbg_aiobuf_stats (&sjob->aio.mdat, "MCA data", self->id);
		s_task_save_dbg_aiobuf_stats (&sjob->aio.tdat, "Tick data", self->id);
		s_task_save_dbg_aiobuf_stats (&sjob->aio.edat, "Event data", self->id);
#endif
		s_task_save_dbg_aiobuf_stats (&sjob->aio.fidx, "Frame index", self->id);
		s_task_save_dbg_aiobuf_stats (&sjob->aio.tidx, "Tick index", self->id);
		s_task_save_dbg_aiobuf_stats (&sjob->aio.midx, "MCA index", self->id);
		s_task_save_dbg_aiobuf_stats (&sjob->aio.ridx, "Trace index", self->id);
#endif
		/* Close stream and index files. */
		s_task_save_close (sjob);

		/* Write and send stats. */
		int rc = s_task_save_stats_write  (sjob);
		if (rc != 0)
		{
			s_msg (errno, LOG_ERR, self->id,
				"Could not write stats");
		}
		s_task_save_stats_send  (sjob, self->frontend);

		/* Enable polling on the reader and deactivate packet
		 * handler. */
		return TASK_SLEEP;
	}

	return 0;
}

/*
 * Perform checks and statically allocate the data struct.
 * mmap data for stream and index files.
 * Returns 0 on success, -1 on error.
 */
static int
s_task_save_init (task_t* self)
{
	assert (*(TSAVE_ROOT + strlen (TSAVE_ROOT) - 1) == '/');
	assert (sizeof (struct s_task_save_stats_t) == TSAVE_STAT_LEN);
	assert (sizeof (struct s_task_save_fidx_t) == TSAVE_FIDX_LEN);
	assert (sizeof (struct s_task_save_tidx_t) == TSAVE_TIDX_LEN);
	assert (sizeof (struct s_task_save_sidx_t) == TSAVE_SIDX_LEN);
	assert (self != NULL);

	static struct s_task_save_data_t sjob;
	sjob.fd = -1;

	int rc = 0;
#ifdef TSAVE_SINGLE_FILE
	rc |= s_task_save_init_aiobuf (&sjob.aio.dat);
#else
	rc |= s_task_save_init_aiobuf (&sjob.aio.bdat);
	rc |= s_task_save_init_aiobuf (&sjob.aio.mdat);
	rc |= s_task_save_init_aiobuf (&sjob.aio.tdat);
	rc |= s_task_save_init_aiobuf (&sjob.aio.edat);
#endif
	rc |= s_task_save_init_aiobuf (&sjob.aio.fidx);
	rc |= s_task_save_init_aiobuf (&sjob.aio.midx);
	rc |= s_task_save_init_aiobuf (&sjob.aio.tidx);
	rc |= s_task_save_init_aiobuf (&sjob.aio.ridx);
	if (rc != 0)
	{
		s_msg (errno, LOG_ERR, self->id,
			"Cannot mmap %lu bytes", TSAVE_BUFSIZE);
		return -1;
	}

	self->data = &sjob;
	return 0;
}

/*
 * Send off stats for any ongoing job. Close all files.
 * Unmap data for stream and index files.
 * Returns 0 on success, -1 if job status could not be sent or written.
 */
static int
s_task_save_fin (task_t* self)
{
	assert (self != NULL);

	struct s_task_save_data_t* sjob =
		(struct s_task_save_data_t*) self->data;
	assert (sjob != NULL);

	int rc = 0;
	if (sjob->filename != NULL)
	{ /* A job was in progress. _stats_send nullifies this. */
		s_task_save_flush (sjob);
		s_task_save_close (sjob);
		rc  = s_task_save_stats_write (sjob);
		rc |= s_task_save_stats_send  (sjob, self->frontend);
	}

#ifdef TSAVE_SINGLE_FILE
	s_task_save_fin_aiobuf (&sjob->aio.dat);
#else
	s_task_save_fin_aiobuf (&sjob->aio.bdat);
	s_task_save_fin_aiobuf (&sjob->aio.mdat);
	s_task_save_fin_aiobuf (&sjob->aio.tdat);
	s_task_save_fin_aiobuf (&sjob->aio.edat);
#endif
	s_task_save_fin_aiobuf (&sjob->aio.fidx);
	s_task_save_fin_aiobuf (&sjob->aio.midx);
	s_task_save_fin_aiobuf (&sjob->aio.tidx);
	s_task_save_fin_aiobuf (&sjob->aio.ridx);

	self->data = NULL;
	return (rc ? -1 : 0);
}

/*
 * mmap data for a stream or index file.
 * Returns 0 on success, -1 on error.
 */
static int
s_task_save_init_aiobuf (struct s_task_save_aiobuf_t* aiobuf)
{
	assert (aiobuf != NULL);

	aiobuf->aios.aio_sigevent.sigev_notify = SIGEV_NONE;
	aiobuf->aios.aio_fildes = -1;

	void* buf = mmap (NULL, TSAVE_BUFSIZE, PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == (void*)-1)
		return -1;

	aiobuf->bufzone.base = aiobuf->bufzone.tail =
		aiobuf->bufzone.cur = (unsigned char*) buf;
	aiobuf->bufzone.ceil = aiobuf->bufzone.base + TSAVE_BUFSIZE;

	return 0;
}

/*
 * munmap data for a stream or index file.
 */
static void
s_task_save_fin_aiobuf (struct s_task_save_aiobuf_t* aiobuf)
{
	assert (aiobuf != NULL);

	/* Unmap bufzone */
	if (aiobuf->bufzone.base != NULL)
	{
		munmap (aiobuf->bufzone.base, TSAVE_BUFSIZE);
		aiobuf->bufzone.base = NULL;
	}
}

/*
 * Opens the stream and index files.
 * It does not close any successfully opened files are closed if an error occurs.
 * Returns 0 on success, -1 on error.
 */
static int
s_task_save_open (struct s_task_save_data_t* sjob, mode_t fmode)
{
	assert (sjob != NULL);
	assert (sjob->filename != NULL);

	dbg_assert (sjob->st.ticks == 0);
	dbg_assert (sjob->st.events == 0);
	dbg_assert (sjob->st.traces == 0);
	dbg_assert (sjob->st.hists == 0);
	dbg_assert (sjob->st.frames == 0);
	dbg_assert (sjob->st.frames_lost == 0);
	dbg_assert (sjob->st.frames_dropped == 0);
	dbg_assert (sjob->st.errors == 0);

	dbg_assert (sjob->cur_stream.size == 0);
	dbg_assert (sjob->cur_stream.cur_size == 0);
	dbg_assert (sjob->cur_tick.nframes == 0);

	/* Open the data files. */
	if (strlen (sjob->filename) + 6 > PATH_MAX)
	{
		errno = ENAMETOOLONG;
		return -1;
	}

	int rc;
	char buf[PATH_MAX];
	memset (buf, 0, PATH_MAX);
	strcpy (buf, sjob->filename);
	char* ext = buf + strlen (buf);

	rc = 0;
#ifdef TSAVE_SINGLE_FILE
	strcpy (ext, ".dat");
	rc |= s_task_save_open_aiobuf (&sjob->aio.dat, buf, fmode);
#else
	strcpy (ext, ".bdat");
	rc |= s_task_save_open_aiobuf (&sjob->aio.bdat, buf, fmode);
	strcpy (ext, ".mdat");
	rc |= s_task_save_open_aiobuf (&sjob->aio.mdat, buf, fmode);
	strcpy (ext, ".tdat");
	rc |= s_task_save_open_aiobuf (&sjob->aio.tdat, buf, fmode);
	strcpy (ext, ".edat");
	rc |= s_task_save_open_aiobuf (&sjob->aio.edat, buf, fmode);
#endif
	strcpy (ext, ".fidx");
	rc |= s_task_save_open_aiobuf (&sjob->aio.fidx, buf, fmode);
	strcpy (ext, ".midx");
	rc |= s_task_save_open_aiobuf (&sjob->aio.midx, buf, fmode);
	strcpy (ext, ".tidx");
	rc |= s_task_save_open_aiobuf (&sjob->aio.tidx, buf, fmode);
	strcpy (ext, ".ridx");
	rc |= s_task_save_open_aiobuf (&sjob->aio.ridx, buf, fmode);

	if (rc != 0)
		return -1;

	return 0;
}

/*
 * Closes the stream and index files.
 */
static void
s_task_save_close (struct s_task_save_data_t* sjob)
{
	assert (sjob != NULL);
	assert (sjob->filename != NULL);

	/* Close the data files. */
#ifdef TSAVE_SINGLE_FILE
	s_task_save_close_aiobuf (&sjob->aio.dat);
#else
	s_task_save_close_aiobuf (&sjob->aio.bdat);
	s_task_save_close_aiobuf (&sjob->aio.mdat);
	s_task_save_close_aiobuf (&sjob->aio.tdat);
	s_task_save_close_aiobuf (&sjob->aio.edat);
#endif
	s_task_save_close_aiobuf (&sjob->aio.fidx);
	s_task_save_close_aiobuf (&sjob->aio.midx);
	s_task_save_close_aiobuf (&sjob->aio.tidx);
	s_task_save_close_aiobuf (&sjob->aio.ridx);
}

/*
 * Open a stream or index file.
 * Returns 0 on success, -1 on error.
 */
static int
s_task_save_open_aiobuf (struct s_task_save_aiobuf_t* aiobuf,
	const char* filename, mode_t fmode)
{
	assert (aiobuf != NULL);

	dbg_assert (aiobuf->aios.aio_fildes == -1);
	dbg_assert (aiobuf->size == 0);
	dbg_assert (aiobuf->bufzone.cur == aiobuf->bufzone.tail);
	dbg_assert (aiobuf->bufzone.cur == aiobuf->bufzone.base);
	dbg_assert (aiobuf->bufzone.waiting == 0);
	dbg_assert (aiobuf->bufzone.enqueued == 0);

	aiobuf->aios.aio_fildes = open (filename, fmode,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (aiobuf->aios.aio_fildes == -1)
		return -1;

	return 0;
}

/*
 * Close a stream or index file. Reset cursor and tail of bufzone.
 * Zero the aiocb struct.
 */
static void
s_task_save_close_aiobuf (struct s_task_save_aiobuf_t* aiobuf)
{
	assert (aiobuf != NULL);

	if (aiobuf->aios.aio_fildes == -1)
		return; /* _open failed? */

	aiobuf->bufzone.waiting = 0;
	aiobuf->bufzone.enqueued = 0;
#ifdef ENABLE_FULL_DEBUG
	memset (&aiobuf->bufzone.st, 0, sizeof(aiobuf->bufzone.st));
#endif

	ftruncate (aiobuf->aios.aio_fildes, aiobuf->size);
	close (aiobuf->aios.aio_fildes);
	memset (&aiobuf->aios, 0, sizeof(aiobuf->aios));
	aiobuf->aios.aio_sigevent.sigev_notify = SIGEV_NONE;
	aiobuf->aios.aio_fildes = -1;

	aiobuf->size = 0;

	aiobuf->bufzone.cur = aiobuf->bufzone.tail =
		aiobuf->bufzone.base;
}

/*
 * Opens the stats file and reads stats. Closes it afterwards.
 * Returns 0 on success, -1 on error.
 */
static int
s_task_save_stats_read (struct s_task_save_data_t* sjob)
{
	assert (sjob != NULL);
	assert (sjob->filename != NULL);
	dbg_assert (sjob->fd == -1);

	sjob->fd = open (sjob->filename, O_RDONLY);
	if (sjob->fd == -1)
		return -1;

	off_t rc = read (sjob->fd, &sjob->st, TSAVE_STAT_LEN);
	close (sjob->fd);
	sjob->fd = -1;

	if (rc != TSAVE_STAT_LEN)
		return -1;
	
	return 0;
}

/*
 * Opens the stats file and writes stats. Closes it afterwards.
 * Returns 0 on success, -1 on error.
 */
static int
s_task_save_stats_write (struct s_task_save_data_t* sjob)
{
	assert (sjob != NULL);
	assert (sjob->filename != NULL);
	dbg_assert (sjob->fd == -1);

	sjob->fd = open (sjob->filename, O_WRONLY | O_CREAT,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (sjob->fd == -1)
		return -1;

	off_t rc = write (sjob->fd, &sjob->st, TSAVE_STAT_LEN);
	close (sjob->fd);
	sjob->fd = -1;

	if (rc != TSAVE_STAT_LEN)
		return -1;
	
	return 0;
}

/*
 * Sends the statistics to the client and resets them.
 * Returns 0 on success, -1 on error.
 */
static int
s_task_save_stats_send (struct s_task_save_data_t* sjob, zsock_t* frontend)
{
	assert (sjob != NULL);
	assert (sjob->filename != NULL);
	dbg_assert (sjob->fd == -1); /* _read and _write should close it */

	int rc = zsock_send (frontend, TSAVE_REP_PIC,
			(sjob->min_ticks > sjob->st.ticks ||
				 sjob->min_events > sjob->st.events ) ?
				TSAVE_REQ_ERR : TSAVE_REQ_OK,
			sjob->st.ticks,
			sjob->st.events,
			sjob->st.traces,
			sjob->st.hists,
			sjob->st.frames,
			sjob->st.frames_lost,
			sjob->st.frames_dropped);

	memset (&sjob->st, 0, TSAVE_STAT_LEN);
	memset (&sjob->cur_stream, 0, sizeof (sjob->cur_stream));
	memset (&sjob->cur_tick, 0, sizeof (sjob->cur_tick));

	sjob->filename = NULL; /* points to a static string */
	sjob->recording = 0;

	return rc;
}

/*
 * Blocks untils the aio jobs for all bufzones are ready.
 */
static void
s_task_save_flush (struct s_task_save_data_t* sjob)
{
	assert (sjob != NULL);

	int jobrc;
#ifdef TSAVE_SINGLE_FILE
	do {
		jobrc = s_task_save_queue_aiobuf (&sjob->aio.dat, 1);
	} while (jobrc == EINPROGRESS);
#else
	do {
		jobrc = s_task_save_queue_aiobuf (&sjob->aio.bdat, 1);
	} while (jobrc == EINPROGRESS);
	do {
		jobrc = s_task_save_queue_aiobuf (&sjob->aio.mdat, 1);
	} while (jobrc == EINPROGRESS);
	do {
		jobrc = s_task_save_queue_aiobuf (&sjob->aio.tdat, 1);
	} while (jobrc == EINPROGRESS);
	do {
		jobrc = s_task_save_queue_aiobuf (&sjob->aio.edat, 1);
	} while (jobrc == EINPROGRESS);
#endif
	do {
		jobrc = s_task_save_queue_aiobuf (&sjob->aio.fidx, 1);
	} while (jobrc == EINPROGRESS);
	do {
		jobrc = s_task_save_queue_aiobuf (&sjob->aio.midx, 1);
	} while (jobrc == EINPROGRESS);
	do {
		jobrc = s_task_save_queue_aiobuf (&sjob->aio.tidx, 1);
	} while (jobrc == EINPROGRESS);
	do {
		jobrc = s_task_save_queue_aiobuf (&sjob->aio.ridx, 1);
	} while (jobrc == EINPROGRESS);
}

/*
 * Copies buf to bufzone. If previous aio_write is completed and enough bytes
 * are waiting in buffer, queues them.
 * If there is no space for another packet, will block until it's done.
 * Returns 0 on success or if nothing was queued.
 * Otherwise returns same as s_task_save_queue_aiobuf.
 */
static int
s_task_save_try_queue_aiobuf (struct s_task_save_aiobuf_t* aiobuf,
	const char* buf, uint16_t len, int task_id)
{
	dbg_assert (aiobuf != NULL);
	dbg_assert (aiobuf->aios.aio_fildes != -1);
	dbg_assert (buf != NULL);
	dbg_assert (len > 0);

	dbg_assert (aiobuf->bufzone.enqueued + aiobuf->bufzone.waiting <=
		TSAVE_BUFSIZE - MAX_TES_FRAME_LEN);
	dbg_assert (aiobuf->bufzone.cur >= aiobuf->bufzone.base);
	dbg_assert (aiobuf->bufzone.tail >= aiobuf->bufzone.base);
	dbg_assert (aiobuf->bufzone.cur < aiobuf->bufzone.ceil);
	dbg_assert (aiobuf->bufzone.tail + aiobuf->bufzone.enqueued <=
		aiobuf->bufzone.ceil);
	dbg_assert (aiobuf->bufzone.cur < aiobuf->bufzone.tail ||
		aiobuf->bufzone.cur >=
			aiobuf->bufzone.tail + aiobuf->bufzone.enqueued);
	dbg_assert (aiobuf->bufzone.cur == aiobuf->bufzone.tail
		+ aiobuf->bufzone.enqueued + aiobuf->bufzone.waiting -
		((aiobuf->bufzone.cur < aiobuf->bufzone.tail) ?
			 TSAVE_BUFSIZE : 0));

	/* Wrap cursor if needed */
	int reserve = len - (aiobuf->bufzone.ceil - aiobuf->bufzone.cur);
	if (likely (reserve < 0))
	{
		memcpy (aiobuf->bufzone.cur, buf, len); 
		aiobuf->bufzone.cur += len;
	}
	else
	{
		memcpy (aiobuf->bufzone.cur, buf, len - reserve); 
		if (reserve > 0)
			memcpy (aiobuf->bufzone.base,
					buf + len - reserve, reserve); 
		aiobuf->bufzone.cur = aiobuf->bufzone.base + reserve;
	}
	aiobuf->bufzone.waiting += len;

#if 1 /* 0 to skip writing */
	/* If there is < MINSIZE waiting and the cursor hasn't wrapped and
	 * there is stil space for more packets, wait. */
	if (aiobuf->bufzone.waiting < TSAVE_MINSIZE && reserve < 0 &&
		aiobuf->bufzone.enqueued + aiobuf->bufzone.waiting <=
			TSAVE_BUFSIZE - MAX_TES_FRAME_LEN)
		return 0;

	/* Try to queue next batch but don't force */
	int jobrc = s_task_save_queue_aiobuf (aiobuf, 0);
#ifdef ENABLE_FULL_DEBUG
	if (jobrc == EINPROGRESS)
		aiobuf->bufzone.st.num_skipped++;
#endif

	/* If there is no space for a full frame, force write until there is.
	 * If we are finalizingm wait for all bytes to be written. */
#ifdef ENABLE_FULL_DEBUG
	bool blocked = 0;
#endif
	while ( aiobuf->bufzone.enqueued + aiobuf->bufzone.waiting >
			TSAVE_BUFSIZE - MAX_TES_FRAME_LEN &&
		jobrc == EINPROGRESS )
	{
#ifdef ENABLE_FULL_DEBUG
		blocked = 1;
#endif
		jobrc = s_task_save_queue_aiobuf (aiobuf, 1);
	}
#ifdef ENABLE_FULL_DEBUG
	if (blocked)
		aiobuf->bufzone.st.num_blocked++;
#endif
	if (jobrc == -1)
	{
		/* TO DO: how to handle errors */
		s_msg (errno, LOG_ERR, task_id, "Could not write to file");
	}
	else if (jobrc == -2)
	{
		/* TO DO: how to handle errors */
#ifdef ENABLE_FULL_DEBUG
		s_msg (0, LOG_ERR, task_id,
			"Queued %lu bytes, wrote %lu",
			aiobuf->bufzone.enqueued,
			aiobuf->bufzone.st.last_written);
#else /* ENABLE_FULL_DEBUG */
		s_msg (0, LOG_ERR, task_id,
			"Wrote unexpected number of bytes");
#endif /* ENABLE_FULL_DEBUG */
	}

#else /* skip writing */
	int jobrc = 0;
	aiobuf->size += aiobuf->bufzone.waiting;
	aiobuf->bufzone.waiting = 0;
	aiobuf->bufzone.tail = aiobuf->bufzone.cur;
#endif /* skip writing */

	dbg_assert (aiobuf->bufzone.enqueued + aiobuf->bufzone.waiting <=
		TSAVE_BUFSIZE - MAX_TES_FRAME_LEN);
	return jobrc;
}

/*
 * Queue the next batch for aio_write-ing.
 * If force is true, will suspend if file is not ready for writing.
 * Always calls aio_return for previous job. Calls aio_return if waiting for
 * new job.
 * 
 * Returns 0 if no new bytes in the bufzone (should only happen if flushing or
 * waiting for a large batch and no space in the bufzone).
 * Returns EINPROGRESS on successful queue, or if force is false and file is
 * not ready.
 * Returns -1 on error.
 * Returns -2 if number of bytes written as reported by aio_return is
 * unexpected.
 */
static int
s_task_save_queue_aiobuf (struct s_task_save_aiobuf_t* aiobuf, bool force)
{
	dbg_assert (aiobuf != NULL);

	/* If there was no previous job, no need to do checks. */
	if (aiobuf->bufzone.enqueued == 0)
		goto prepare_next;

	/* ----------------------------------------------------------------- */
	/* Check if ready. */
	int rc = aio_error (&aiobuf->aios);
	if ( ! force && rc == EINPROGRESS )
		return EINPROGRESS;

	/* Suspend while ready. */
	if ( rc == EINPROGRESS )
	{
		const struct aiocb* aiol = &aiobuf->aios;
		rc = aio_suspend (&aiol, 1, NULL);
		if (rc == -1)
			return -1;
		rc = aio_error (&aiobuf->aios);
	}

	if (rc != 0)
	{
		dbg_assert (rc != ECANCELED && rc != EINPROGRESS);
		errno = rc; /* aio_error does not set it */
		return -1;
	}

	/* Check completion status. */
	ssize_t wrc = aio_return (&aiobuf->aios);
	if (wrc == -1 && errno == EAGAIN)
	{
#ifdef ENABLE_FULL_DEBUG
		aiobuf->bufzone.st.failed_batches++;
#endif
		goto queue_as_is; /* requeue previous batch */
	}

	if (wrc == -1)
		return -1; /* an error other than EAGAIN */
	if ((size_t)wrc != aiobuf->bufzone.enqueued)
	{
		dbg_assert (aiobuf->bufzone.enqueued > 0);
#ifdef ENABLE_FULL_DEBUG
		aiobuf->bufzone.st.last_written = wrc;
#endif
		return -2;
	}

	/* ----------------------------------------------------------------- */
prepare_next:
#ifdef ENABLE_FULL_DEBUG
	{
		int bin = aiobuf->bufzone.enqueued * (TSAVE_HISTBINS - 1) / TSAVE_BUFSIZE;
		dbg_assert (bin >= 0 && bin < TSAVE_HISTBINS);
		aiobuf->bufzone.st.batches[bin]++;
	}
	aiobuf->bufzone.st.prev_waiting = aiobuf->bufzone.waiting;
	aiobuf->bufzone.st.prev_enqueued = aiobuf->bufzone.enqueued;
#endif

	/* Increase file size by number of bytes written. */
	aiobuf->size += aiobuf->bufzone.enqueued;

	/* Release written bytes by moving the tail. */
	aiobuf->bufzone.tail += aiobuf->bufzone.enqueued;
	/* if cursor had wrapped around last time */
	if (aiobuf->bufzone.tail == aiobuf->bufzone.ceil)
		aiobuf->bufzone.tail = aiobuf->bufzone.base;
	dbg_assert (aiobuf->bufzone.tail < aiobuf->bufzone.ceil);

	/* If cursor had wrapped around, queue until the end of the
	 * bufzone. When done, tail will move to ceil, we handle
	 * this above. */
	if (unlikely (aiobuf->bufzone.cur < aiobuf->bufzone.tail))
		aiobuf->bufzone.enqueued = aiobuf->bufzone.ceil
			- aiobuf->bufzone.tail;
	else
		aiobuf->bufzone.enqueued = aiobuf->bufzone.cur
			- aiobuf->bufzone.tail;

	dbg_assert (aiobuf->bufzone.waiting >= aiobuf->bufzone.enqueued);
	aiobuf->bufzone.waiting -= aiobuf->bufzone.enqueued;

	dbg_assert (aiobuf->bufzone.waiting == 0 || aiobuf->bufzone.tail
			+ aiobuf->bufzone.enqueued == aiobuf->bufzone.ceil);

	/* ----------------------------------------------------------------- */
queue_as_is:
	dbg_assert (aiobuf->bufzone.tail != aiobuf->bufzone.ceil);
	/* Check if called in vain, should only happen at the end when flushing or
	 * if we had queued a batch larger than TSAVE_BUFSIZE - MAX_TES_FRAME_LEN. */
	if (aiobuf->bufzone.enqueued == 0)
	{
		dbg_assert (aiobuf->bufzone.waiting == 0);
		return 0;
	}

	aiobuf->aios.aio_offset = aiobuf->size;
	aiobuf->aios.aio_buf = aiobuf->bufzone.tail;
	aiobuf->aios.aio_nbytes = aiobuf->bufzone.enqueued;
	do
	{
		rc = aio_write (&aiobuf->aios);
	} while (rc == -1 && errno == EAGAIN);
	if (rc == -1)
		return -1; /* an error other than EAGAIN */
	return EINPROGRESS;
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
s_task_save_canonicalize_path (const char* filename, bool checkonly, int task_id)
{
	assert (filename != NULL);

	errno = 0;
	size_t len = strlen (filename);
	if (len == 0)
	{
		s_msg (0, LOG_DEBUG, task_id, "Filename is empty");
		return NULL;
	}

#ifdef TSAVE_REQUIRE_FILENAME
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
		assert (rs == finalpath);
		if ( memcmp (finalpath, TSAVE_ROOT, strlen (TSAVE_ROOT)) != 0)
		{
			s_msg (0, LOG_DEBUG, task_id,
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
		assert (len < PATH_MAX);
		if (len + next_seg - cur_seg + 1 >= PATH_MAX)
		{
			s_msg (0, LOG_DEBUG, task_id,
				"Filename too long");
			return NULL;
		}
		strncpy (buf + len, cur_seg, next_seg - cur_seg + 1);
		len += next_seg - cur_seg + 1;
		assert (len == strlen (buf));

		errno = 0;
		int rc = mkdir (buf, 0777);
		if (rc && errno != EEXIST)
			return NULL; /* don't handle other errors */

		cur_seg = next_seg + 1; /* skip over leading slash */
	}

	/* Canonicalize the directory part */
	rs = realpath (buf, finalpath);
	assert (rs != NULL); /* this shouldn't happen */
	assert (rs == finalpath);

	/* Add the base filename (realpath removes the trailing slash) */
#ifdef TSAVE_REQUIRE_FILENAME
	assert (strlen (cur_seg) > 0);
#else
	/* TO DO: generate a filename is none is given */
	assert (0);
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
		s_msg (0, LOG_DEBUG, task_id,
				"Resolved to %s, outside of root",
				finalpath);
		return NULL; /* outside of root */
	}

	return finalpath;
}

#ifdef ENABLE_FULL_DEBUG
static void
s_task_save_dbg_aiobuf_stats (struct s_task_save_aiobuf_t* aiobuf,
		const char* stream, int task_id)
{
	s_msg (0, LOG_DEBUG, task_id, "%s stream: ", stream); 
	uint64_t batches_tot = 0, steps = TSAVE_BUFSIZE / (TSAVE_HISTBINS - 1);
	for (int b = 0 ; b < TSAVE_HISTBINS ; b++)
	{
		s_msg (0, LOG_DEBUG, task_id,
			"     %lu B to %lu B: %lu batches",
			b*steps, (b+1)*steps, aiobuf->bufzone.st.batches[b]);
		batches_tot += aiobuf->bufzone.st.batches[b];
	}

	s_msg (0, LOG_DEBUG, task_id,
		"     Wrote %lu batches (%lu repeated, %lu skipped, %lu blocked)",
		batches_tot, aiobuf->bufzone.st.failed_batches,
		aiobuf->bufzone.st.num_skipped, aiobuf->bufzone.st.num_blocked);
}
#endif

/* ------------------------------------------------------------------------- */
/* ------------------------- GET AVERAGE TRACE TASK ------------------------ */
/* ------------------------------------------------------------------------- */

static int
s_task_avgtr_req_hn (zloop_t* loop, zsock_t* reader, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;

	uint32_t timeout;	

	int rc = zsock_recv (reader, TAVGTR_REQ_PIC, &timeout);
	if (rc == -1)
	{ /* would also return -1 if picture contained a pointer (p) or a null
	   * frame (z) but message received did not match this signature; this
	   * is irrelevant in this case */
		s_msg (0, LOG_DEBUG, self->id, "Receive interrupted");
		return TASK_ERROR;
	}

	/* Check timeout. */
	if (timeout == 0)
	{
		s_msg (0, LOG_INFO, self->id,
			"Received a malformed request");
		zsock_send (self->frontend, TAVGTR_REP_PIC_FAIL,
				TAVGTR_REQ_INV);
		return 0;
	}

	s_msg (0, LOG_INFO, self->id,
		"Received request for a trace in the next %u seconds",
		timeout);

	/* Register a timer */
	int tid = zloop_timer (loop, 1000 * timeout, 1,
		s_task_avgtr_timeout_hn, self);
	if (tid == -1)
	{
		s_msg (errno, LOG_ERR, self->id,
			"Could not set a timer");
		return TASK_ERROR;
	}
	struct s_task_avgtr_data_t* trace =
		(struct s_task_avgtr_data_t*) self->data;
	dbg_assert ( ! trace->recording );
	trace->timer = tid;

	/* Disable polling on the reader until the job is done. Wakeup packet
	 * handler. */
	s_task_activate (self);

	return 0;
}

/*
 * Accumulates average trace frames. As soon as a complete trace is
 * recorded, it is sent to the client, polling on the client reader is
 * re-enabled and the timer is canceled. It aborts the whole trace if
 * a relevant frame is lost, and waits for the next one.
 */
static int
s_task_avgtr_pkt_hn (zloop_t* loop, tespkt* pkt, uint16_t flen,
		uint16_t missed, int err, task_t* self)
{
	dbg_assert (self != NULL);

	if ( ! tespkt_is_trace_avg (pkt) )
		return 0;

	struct s_task_avgtr_data_t* trace =
		(struct s_task_avgtr_data_t*) self->data;

	if ( ! trace->recording && tespkt_is_header (pkt) )
	{ /* start the trace */
		trace->recording = 1;
		trace->size = tespkt_trace_size (pkt);
	}

	if ( ! trace->recording )
		return 0;

	/* We don't handle bad frames, drop trace if bad. */
	int rep = -1;
	if (err)
	{
#ifdef ENABLE_FULL_DEBUG
		s_msg (0, LOG_DEBUG, self->id,
			"Bad frame, error is %d", err);
#endif
		rep = TAVGTR_REQ_ERR;
		goto done;
	}
	
	/* Check protocol sequence for subsequent frames. */
	if (trace->cur_size > 0)
	{
		uint16_t cur_pseq = tespkt_pseq (pkt);
		if ((uint16_t)(cur_pseq - self->prev_pseq_tr) != 1)
		{ /* missed frames */
#ifdef ENABLE_FULL_DEBUG
			s_msg (0, LOG_DEBUG, self->id,
				"Mismatch in protocol sequence after byte %hu",
				trace->cur_size);
#endif
			rep = TAVGTR_REQ_ERR;
			goto done;
		}
	}

	/* Append the data, check current size. */
	uint16_t paylen = flen - TES_HDR_LEN;
	dbg_assert (trace->cur_size <= TAVGTR_MAXSIZE - paylen);
	memcpy (trace->buf + trace->cur_size,
		(char*)pkt + TES_HDR_LEN, paylen);

	trace->cur_size += paylen;
	if (trace->cur_size == trace->size)
	{
	       	rep = TAVGTR_REQ_OK;
		goto done;
	}

	return 0;

done:
	/* Cancel the timer. */
	zloop_timer_end (loop, trace->timer);

	/* Send the trace. */
	switch (rep)
	{
		case TAVGTR_REQ_ERR:
			s_msg (0, LOG_INFO, self->id,
				"Discarded average trace");
			zsock_send (self->frontend, TAVGTR_REP_PIC_FAIL,
					TAVGTR_REQ_ERR);
			break;
		case TAVGTR_REQ_OK:
			s_msg (0, LOG_INFO, self->id,
				"Average trace complete");
			zsock_send (self->frontend, TAVGTR_REP_PIC_OK,
					TAVGTR_REQ_OK, &trace->buf,
					trace->size);
			break;
		default:
			assert (0);
	}

	/* Reset stats. */
	trace->recording = 0;
	trace->cur_size = 0;
	trace->size = 0;

	/* Enable polling on the reader and deactivate packet
	 * handler. */
	return TASK_SLEEP;
}

/*
 * Deactivates the task, enables polling on the client reader, sends
 * timeout error to the client.
 */
static int
s_task_avgtr_timeout_hn (zloop_t* loop, int timer_id, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;

	/* Enable polling on the reader and deactivate packet
	 * handler. */
	s_task_deactivate (self);
	
	/* Send a timeout error to the client. */
	s_msg (0, LOG_INFO, self->id,
		"Average trace timed out");
	zsock_send (self->frontend, TAVGTR_REP_PIC_FAIL,
			TAVGTR_REQ_TOUT);

	return 0;
}

static int
s_task_avgtr_init (task_t* self)
{
	assert (self != NULL);

	static struct s_task_avgtr_data_t trace;

	self->data = &trace;
	return 0;
}

static int
s_task_avgtr_fin (task_t* self)
{
	assert (self != NULL);

	self->data = NULL;
	return 0;
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
s_task_hist_pkt_hn (zloop_t* loop, tespkt* pkt, uint16_t flen,
		uint16_t missed, int err, task_t* self)
{
	dbg_assert (self != NULL);

	if (err)
		return 0; /* we don't handle bad frames */

	if ( ! tespkt_is_mca (pkt) )
		return 0;

	struct s_task_hist_data_t* hist =
		(struct s_task_hist_data_t*) self->data;

	if ( ! tespkt_is_header (pkt) )
	{
		if (hist->discard) 
			return 0;

		/* Check protocol sequence */
		uint16_t cur_pseq = tespkt_pseq (pkt);
		if ((uint16_t)(cur_pseq - self->prev_pseq_mca) != 1)
		{
			s_msg (0, LOG_INFO, self->id,
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
			s_msg (0, LOG_WARNING, self->id,
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
#ifdef ENABLE_FULL_DEBUG
			hist->dropped++;
			s_msg (0, LOG_DEBUG, self->id,
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
		hist->nbins = tespkt_mca_nbins_tot (pkt);
		hist->size  = tespkt_mca_size (pkt);
	}
	dbg_assert ( ! hist->discard );

	hist->cur_nbins += tespkt_mca_nbins (pkt);
	if (hist->cur_nbins > hist->nbins)
	{
		s_msg (0, LOG_WARNING, self->id,
			"Received extra bins: expected %d, so far got %d",
			hist->nbins, hist->cur_nbins);
		hist->discard = 1;
		return 0;
	}

	/* Copy frame, check current size. */
	uint16_t paylen = flen - TES_HDR_LEN;
	dbg_assert (hist->cur_size <= THIST_MAXSIZE - paylen);
	memcpy (hist->buf + hist->cur_size,
		(char*)pkt + TES_HDR_LEN, paylen);

	hist->cur_size += paylen;

	if (hist->cur_nbins == hist->nbins) 
	{
		dbg_assert (hist->cur_size == hist->size);

		/* Send the histogram */
#ifdef ENABLE_FULL_DEBUG
		hist->published++;
		// s_msg (0, LOG_DEBUG, self->id,
		//         "Publishing an %u-byte long histogram",
		//         hist->cur_size);
		int rc = zmq_send (zsock_resolve (self->frontend),
			hist->buf, hist->cur_size, 0);
		if (rc == -1)
		{
			s_msg (errno, LOG_ERR, self->id,
				"Cannot send the histogram");
			return TASK_ERROR;
		}
		if ((unsigned int)rc != hist->cur_size)
		{
			s_msg (errno, LOG_ERR, self->id,
				"Histogram is %lu bytes long, sent %u",
				hist->cur_size, rc);
			return TASK_ERROR;
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

	dbg_assert (hist->cur_size < hist->size);
	return 0;
}

static int
s_task_hist_init (task_t* self)
{
	assert (self != NULL);

	static struct s_task_hist_data_t hist;
	hist.discard = 1;

	self->data = &hist;
	return 0;
}

static int
s_task_hist_fin (task_t* self)
{
	assert (self != NULL);

	self->data = NULL;
	return 0;
}
