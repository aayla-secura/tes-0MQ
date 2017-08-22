/*
 * See api.h
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
 * and each task keeps its own head, which is visible by the coordinator
 * (fpgacoord.c). The coordinator sets the true head to the per-task head which
 * lags behind all others.
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
 *         ifring*     rxring;         // we support only one ring for now
 *         zactor_t*   shim;           // coordinator's end of the pipe, signals
 *                                     // sent on behalf of coordinator go here
 *         zsock_t*    frontend;       // clients
 *         const int   front_type;     // one of ZMQ_*
 *         const char* front_addr;     // the socket addresses, comma separated
 *         int         id;
 *         uint32_t    head;
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
 * from the coordinator. Upon SIG_STOP it exits, upon SIG_WAKEUP it calls
 * calls the task's specific packet handler for each packet in the ring.
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
 * to false. It won't receive SIG_WAKEUP if it is not active and its head won't
 * be synchronized with the real head. When it needs to process packets, it
 * must set its private head to the global head (by calling ifring_head) and
 * then set its active flag to true.
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
 * - Check if packet is valid and drop (increment another counter for malformed
 *   packets).
 */

#include "fpgatasks.h"
#include "net/fpgaif_reader.h"
#include "common.h"
#include "aio.h"

/* From netmap_user.h */
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* ---------------------------------- API ---------------------------------- */

typedef int (s_data_fn)(task_t*);
typedef int (s_pkt_fn)(zloop_t*, fpga_pkt*, uint16_t, task_t*);

/* See DEV NOTES */
struct _task_t
{
	zloop_reader_fn* client_handler;
	s_pkt_fn*        pkt_handler;
	s_data_fn*       data_init;
	s_data_fn*       data_fin;
	void*       data;
	ifring*     rxring;
	zactor_t*   shim;
	zsock_t*    frontend;
	const int   front_type;
	const char* front_addr;
	int         id;
	uint32_t    head;
	uint16_t    prev_fseq;
	uint16_t    prev_pseq_mca;
	uint16_t    prev_pseq_tr;
	uint16_t    prev_pseq_pls;
	bool        error;
	bool        busy;
	bool        active;
	bool        autoactivate;
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

static int  s_task_start (ifring* rxring, task_t* self);
static void s_task_stop (task_t* self);
static inline void s_task_activate (task_t* self);

/* --------------------------- SAVE-TO-FILE TASK --------------------------- */

/* See api.h */
#define REQ_FAIL        0
#define REQ_OK          1
#define REQ_PIC      "s81"
#define REP_PIC    "18888"

#define TSAVE_SOFFSET  40 // beginning of file reserved for statistics
/* Employ a buffer zone for asynchronous writing. We memcpy frames into the
 * bufzone, between its head and cursor (see s_task_save_data_t below) and
 * queue batches with aio_write. */
#define TSAVE_BUFSIZE  15728640UL // 15 MB 

/*
 * Statistics sent as a reply and saved to the file. 
 */
struct s_task_save_stats_t
{	uint64_t ticks;
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
		uint64_t waiting;    // copied into buffer since the last aio_write
		uint64_t enqueued;   // queued for writing at the last aio_write
		unsigned char* base; // mmapped, size of TSAVE_BUFSIZE
		unsigned char* tail; // start address queued for aio_write
		unsigned char* cur;  // address where next packet will be coppied to
		unsigned char* ceil; // base + TSAVE_BUFSIZE
	} bufzone;
	uint64_t max_ticks;
#ifdef FULL_DBG
	uint64_t prev_enqueued;
	uint64_t prev_waiting;
	uint64_t batches;
	uint64_t failed_batches;
	uint64_t num_cleared;
	uint64_t last_written;
	unsigned char prev_hdr[FPGA_HDR_LEN];
#endif
	char*    filename;
};

static zloop_reader_fn s_task_save_req_hn;
static s_pkt_fn        s_task_save_pkt_hn;
static s_data_fn       s_task_save_init;
static s_data_fn       s_task_save_fin;

static int  s_task_save_open  (struct s_task_save_data_t* sjob, mode_t fmode);
static int  s_task_save_queue (struct s_task_save_data_t* sjob, bool force);
static int  s_task_save_read  (struct s_task_save_data_t* sjob);
static int  s_task_save_write (struct s_task_save_data_t* sjob);
static int  s_task_save_send  (struct s_task_save_data_t* sjob,
	zsock_t* frontend);
static void s_task_save_close (struct s_task_save_data_t* sjob);

/* --------------------------- PUBLISH HIST TASK --------------------------- */

#define THIST_MAXSIZE UINT16_MAX

/*
 * Data for currently built histogram.
 */
struct s_task_hist_data_t
{
	uint64_t      dropped;   // number of aborted histograms
	int      nbins;     // total number of bins in histogram
	int      size;      // size of histogram including header
	int      cur_nbins; // number of received bins so far
	int      cur_size;  // number of received bytes so far
	bool          discard;   // discard all frames until the next header
	unsigned char buf[THIST_MAXSIZE];
};

/* There is no handler for the PUB socket. */
static s_pkt_fn        s_task_hist_pkt_hn;
static s_data_fn       s_task_hist_init;
// static s_data_fn       s_task_hist_fin;

/* ----------------------------- THE FULL LIST ----------------------------- */

#define NUM_TASKS 2
static task_t tasks[] = {
	{ // SAVE TO FILE 
		.client_handler = s_task_save_req_hn,
		.pkt_handler    = s_task_save_pkt_hn,
		.data_init      = s_task_save_init,
		.data_fin       = s_task_save_fin,
		.front_type     = ZMQ_REP,
		.front_addr     = "tcp://*:55555",
	},
	{ // PUBLISH HIST
		.pkt_handler    = s_task_hist_pkt_hn,
		.data_init      = s_task_hist_init,
		.front_type     = ZMQ_PUB,
		.front_addr     = "tcp://*:55556",
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
tasks_start (ifring* rxring, zloop_t* c_loop)
{
	dbg_assert (rxring != NULL);
	dbg_assert (NUM_TASKS == sizeof (tasks)/sizeof (task_t));
	int rc;
	for (int t = 0; t < NUM_TASKS; t++)
	{
		tasks[t].id = t + 1;
		s_msgf (0, LOG_DEBUG, 0, "Starting task #%d", t);
		rc = s_task_start (rxring, &tasks[t]);
		if (rc)
		{
			s_msg (errno, LOG_ERR, 0, "Could not start tasks");
			return -1;
		}
	}

	if (c_loop)
		return tasks_read (c_loop);
	return 0;
}

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

void
tasks_get_head (uint32_t* head)
{
	for (int t = 0; t < NUM_TASKS; t++)
	{
		task_t* self = &tasks[t];
		if (self->active)
			*head = ifring_earlier_id (self->rxring,
				*head, self->head);
	}
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
	dbg_assert (self->busy == 0);
	
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
	// dbg_assert (self->active == 0);
	/* FIX: signals seem to arrive with a short delay, so right after
	 * setting active to false, or reaching the ring's tail we may receive
	 * a WAKEUP. */
	if ( self->active == 0 || self->head == ifring_tail (self->rxring) )
		return 0;

	self->busy = 1;
	/* Process packets */
	do
	{
		fpga_pkt* pkt = (fpga_pkt*) ifring_buf (
			self->rxring, self->head);

		/* TO DO: check packet */
		uint16_t len = ifring_len (self->rxring, self->head);
		uint16_t plen = pkt_len (pkt);
		if (plen > len)
		{ /* drop the frame */
			s_msgf (0, LOG_DEBUG, self->id,
				"Packet too long (header says %hu, "
				"ring slot is %hu)", plen, len);
			return 0;
		}
		dbg_assert (plen <= MAX_FPGA_FRAME_LEN);
		int rc = self->pkt_handler (loop, pkt, plen, self);

		self->prev_fseq = frame_seq (pkt);
		if (is_mca (pkt))
			self->prev_pseq_mca = proto_seq (pkt);
		else if (is_trace (pkt))
			self->prev_pseq_tr = proto_seq (pkt);
		else
			self->prev_pseq_pls = proto_seq (pkt);

		self->head = ifring_following (self->rxring, self->head);
		if (rc)
			break;

	} while (self->head != ifring_tail (self->rxring));

	if (self->error)
	{
		self->active = 0;
		return -1;
	}

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
		s_msg (0, LOG_DEBUG, 0, "Task thread encountered an error");
		return -1;
	}
	dbg_assert (0); // we only deal with SIG_DIED 
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
	dbg_assert (self->rxring != NULL);
	dbg_assert (self->frontend == NULL);
	dbg_assert (self->id > 0);
	dbg_assert (self->head == 0);
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
	zloop_set_nonstop (loop, 1);

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
}

/*
 * Initializes a task_t and starts a new thread using zactor_new.
 * Registers the task's back end of the pipe with the coordinator's loop.
 * Returns 0 on success, -1 on error.
 */
static int
s_task_start (ifring* rxring, task_t* self)
{
	dbg_assert (self != NULL);
	dbg_assert (rxring != NULL);

	self->rxring = rxring;

	/* Start the thread, will block until the handler signals */
	self->shim = zactor_new (s_task_shim, self);
	dbg_assert (self->shim);
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
		s_msg (0, LOG_DEBUG, self->id, "Task had already exited");
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
	self->head = ifring_head (self->rxring);
	self->active = 1;
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
	dbg_assert (self->busy == 0);
	dbg_assert (self->active == 0);
	struct s_task_save_data_t* sjob =
		(struct s_task_save_data_t*) self->data;
	uint8_t job_mode;

	int rc = zsock_recv (reader, REQ_PIC, &sjob->filename,
		&sjob->max_ticks, &job_mode);
	if (rc == -1)
	{ /* would also return -1 if picture contained a pointer (p) or a null
	   * frame (z) but message received did not match this signature; this
	   * is irrelevant in this case */
		s_msg (0, LOG_DEBUG, self->id, "Receive interrupted");
		self->error = 1;
		return -1;
	}
	if (sjob->filename == NULL || job_mode > 1)
	{
		s_msg (0, LOG_INFO, self->id, "Received a malformed request");
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
		s_msg (0, LOG_INFO, self->id, "Received request for status");
		fmode = O_RDONLY;
		exp_errno = ENOENT;
	}
	else
	{
		s_msgf (0, LOG_INFO, self->id,
			"Received request to write %lu ticks",
			sjob->max_ticks);
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
				"Could not open file %s", sjob->filename);
		}
		s_msg (0, LOG_INFO, self->id, "Not writing to file");
		zsock_send (reader, REP_PIC, REQ_FAIL, 0, 0, 0, 0);
		s_task_save_close (sjob);
		return 0;
	}
	s_msgf (0, LOG_INFO, self->id, "Opened file %s for %s",
		sjob->filename, (fmode & O_RDWR ) ? "writing" : "reading" );

	if (sjob->max_ticks == 0)
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
s_task_save_pkt_hn (zloop_t* loop, fpga_pkt* pkt, uint16_t plen, task_t* self)
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
	dbg_assert (sjob->bufzone.cur = sjob->bufzone.tail
		+ sjob->bufzone.enqueued + sjob->bufzone.waiting -
		((sjob->bufzone.cur < sjob->bufzone.tail) ? TSAVE_BUFSIZE : 0));

	/* TO DO: save err flags */
	/* Update statistics. Size is updated in batches as write operations
	 * finish. */
	uint16_t cur_fseq = frame_seq (pkt);
	if (sjob->st.frames > 0)
		sjob->st.frames_lost += (uint64_t) (
				(uint16_t)(cur_fseq - self->prev_fseq) - 1);
#ifdef FULL_DBG
	if (sjob->st.frames_lost)
	{
		s_msgf (0, LOG_DEBUG, self->id, "Head at: %hu, %hu -> %hu",
			self->head, self->prev_fseq, cur_fseq);
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
		s_msg (errno, LOG_ERR, self->id, "Could not write to file");
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
	dbg_assert (sizeof (struct s_task_save_stats_t) == TSAVE_SOFFSET);
	dbg_assert (self != NULL);

	static struct s_task_save_data_t data;
	data.aios.aio_sigevent.sigev_notify = SIGEV_NONE;
	data.aios.aio_fildes = -1;

	void* buf = mmap (NULL, TSAVE_BUFSIZE, PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == (void*)-1)
	{
		s_msgf (errno, LOG_ERR, self->id,
			"Cannot mmap %lu bytes", TSAVE_BUFSIZE);
		return -1;
	}
	data.bufzone.base = data.bufzone.tail =
		data.bufzone.cur = (unsigned char*) buf;
	data.bufzone.ceil = data.bufzone.base + TSAVE_BUFSIZE;

	self->data = &data;
	return 0;
}

static int
s_task_save_fin (task_t* self)
{
	dbg_assert (self != NULL);
	dbg_assert (self->active == 0);
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
		/* free (sjob->bufzone.base); */
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
	if (rc == (off_t)-1 || rc < TSAVE_SOFFSET)
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
	rc = aio_return (&sjob->aios);
	/* FIX */
	if (rc == -1 && errno == EAGAIN)
	{
#ifdef FULL_DBG
		sjob->failed_batches++;
#endif
		goto queue_as_is; /* requeue previous batch */
	}

	if (rc == -1)
		return -1; /* an error other than EAGAIN */
	if (rc != sjob->bufzone.enqueued)
	{
		dbg_assert (sjob->bufzone.enqueued > 0);
#ifdef FULL_DBG
		sjob->last_written = rc;
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

	int rc = lseek (sjob->aios.aio_fildes, 0, 0);
	if (rc)
		return -1;
	
	rc = read (sjob->aios.aio_fildes, &sjob->st, TSAVE_SOFFSET);
	if (rc < TSAVE_SOFFSET)
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

	int rc = lseek (sjob->aios.aio_fildes, 0, 0);
	if (rc)
		return -1;
	
	rc = write (sjob->aios.aio_fildes, &sjob->st, TSAVE_SOFFSET);
	if (rc < TSAVE_SOFFSET)
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

	zstr_free (&sjob->filename); /* nullifies the pointer */
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

/* ------------------------------------------------------------------------- */
/* --------------------------- PUBLISH HIST TASK --------------------------- */
/* ------------------------------------------------------------------------- */

/*
 * Accumulates MCA frames and sends them out as soon as the last one is
 * received. It aborts the whole histogram if an MCA frame is lost or if extra
 * frames are received (i.e. the size field appears to small).
 */
static int
s_task_hist_pkt_hn (zloop_t* loop, fpga_pkt* pkt, uint16_t plen, task_t* self)
{
	dbg_assert (self != NULL);

	if ( ! is_mca (pkt) )
		return 0;

	struct s_task_hist_data_t* hist =
		(struct s_task_hist_data_t*) self->data;

	if ( ! is_header (pkt) && hist->discard )
		return 0;

	if (is_header (pkt))
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
			hist->dropped++;
		}

		dbg_assert (hist->nbins == 0);
		dbg_assert (hist->size == 0);
		dbg_assert (hist->cur_nbins == 0);
		dbg_assert (hist->cur_size == 0);
		dbg_assert (hist->discard == 0);

		/* Inspect header */
		hist->nbins = mca_num_allbins (pkt);
		hist->size  = mca_size (pkt);

		/* TO DO: move to generic packet checks */
		if (hist->size != hist->nbins * BIN_LEN + MCA_HDR_LEN)
		{
			s_msgf (0, LOG_WARNING, self->id,
				"Size field (%d B) does not match "
				"number of bins (%d)", hist->size, hist->nbins);
			hist->discard = 1;
			return 0;
		}
	}
	dbg_assert ( ! hist->discard );

	/* Check protocol sequence */
	uint16_t cur_pseq = proto_seq (pkt);
	if ( ! is_header (pkt) && (uint16_t)(cur_pseq - self->prev_pseq_mca) != 1)
	{
		s_msgf (0, LOG_INFO, self->id,
			"Frame out of protocol sequence: %hu -> %hu",
			self->prev_pseq_mca, cur_pseq);
		hist->discard = 1;
		return 0;
	}

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
		dbg_assert (hist->cur_size == hist->size);

		/* Send the histogram */
		zframe_t* frame = zframe_new (hist->buf, hist->cur_size);
		/* TO DO: check rc */
		zframe_send (&frame, self->frontend, 0);

		hist->size = 0;
		hist->nbins = 0;
		hist->cur_size = 0;
		hist->cur_nbins = 0;
		return 0;
	}

	dbg_assert (hist->cur_size < hist->size);
}

static int
s_task_hist_init (task_t* self)
{
	dbg_assert (self != NULL);

	static struct s_task_hist_data_t data;
	data.discard = 1;
	self->data = &data;
	return 0;
}
