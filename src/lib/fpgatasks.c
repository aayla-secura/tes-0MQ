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
 *
 * s_task_shim registers a generic reader, s_sig_hn, for handling the signals
 * from the coordinator. Upon SIG_STOP it exits, upon SIG_WAKEUP it calls
 * if_dispatch with the task's specific packet handler.
 * struct _task_t
 * {
 *         zloop_reader_fn* client_handler;
 *         s_pkt_hn*        pkt_handler;
 *         s_data_hn*       task_init_fn; // initialize data
 *         s_data_hn*       task_fin_fn;  // cleanup data
 *         void*     data;         // task-specific
 *         ifring*   rxring;       // we support only one ring for now
 *         zactor_t* shim;         // coordinator's end of the pipe, signals
 *                                 // sent on behalf of the coordinator go here
 *         zsock_t*  frontend;     // clients
 *         const int front_type;   // one of ZMQ_*
 *         const char* front_addr; // the socket addresses, comma separated
 *         int       id;
 *         uint32_t head;
 *         uint16_t cur_frame;
 *         bool      error;        // the frontend and packet handler set this
 *         bool      busy;
 *         bool      active;
 * };
 *
 * If the task defines a public interface address, s_task_shim will open the
 * socket and register the client handler for the task. Each task has a pointer
 * for its own data.
 *
 * Right before entering the loop, s_task_shim will call the task initializer,
 * if it is set. So it can allocate the pointer to its data and do anything
 * else it wishes (talk to clients, etc).
 *
 * Right after the loop terminates, s_task_shim will call the task finalizer,
 * so it can cleanup its data and possibly send final messages to clients.
 *
 * The actual task is done inside client_handler and pkt_handler.
 *
 *   client_handler processes messages on the public socket. If neither
 *   client_handler not front_addr is set, the task has no public interface.
 *
 *   pkt_handler is called by the generic socket reader for each packet in each
 *   ring and does whatever.
 *
 * Both handlers have access to the zloop so they can enable or disable readers
 * (e.g. the client_handler can disable itself after receiving a job and the
 * pkt_handler can re-enable it when done).
 * If either handler encounters an error, it sets the task's error flag to true
 * and exits with -1.
 * pkt_handler may exit with -1 without setting error, if it simply is not
 * interested in more packets for now.
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
#include "api.h" /* defines the public interfaces */
#include "common.h"

/* ---------------------------------- API ---------------------------------- */

typedef int (s_data_hn)(task_t*);
typedef int (s_pkt_hn)(zloop_t*, fpga_pkt*, uint16_t, task_t*);

/* See DEV NOTES */
struct _task_t
{
	zloop_reader_fn* client_handler;
	s_pkt_hn*        pkt_handler;
	s_data_hn*       task_init_fn;
	s_data_hn*       task_fin_fn;
	void*     data;
	ifring*   rxring;
	zactor_t* shim;
	zsock_t*  frontend;
	const int front_type;
	const char* front_addr;
	int       id;
	uint32_t head;
	uint16_t cur_frame;
	bool      error;
	bool      busy;
	bool      active;
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

/* --------------------------- SAVE-TO-FILE TASK --------------------------- */

/* See api.h */
#define REQ_FAIL        0
#define REQ_OK          1
#define REQ_PIC      "s81"
#define REP_PIC    "18888"

#define FDATA_OFF   48 // beginning of file reserved for statistics

/*
 * Statistics saved to the file. 
 */
struct s_task_save_stats_t
{	uint64_t ticks;
	uint64_t size;           // number of written bytes 
	uint64_t frames;
	uint64_t frames_lost;    // missed frames 
	uint64_t frames_dropped; // dropped frames (invalid ones)
	uint64_t errors;         // TO DO: last 8-bytes of the tick header 
};

/*
 * Data for the currently-saved file. max_ticks and filename are set when
 * receiving a request from client.
 */
struct s_task_save_data_t
{
	struct s_task_save_stats_t st;
	uint64_t max_ticks;
	char*     filename;
	int       fd;
};

static zloop_reader_fn s_task_save_req_hn;
static s_pkt_hn        s_task_save_pkt_hn;
static s_data_hn       s_task_save_init;
static s_data_hn       s_task_save_fin;

static int  s_task_save_open (struct s_task_save_data_t* sjob, mode_t fmode);
static int  s_task_save_read (struct s_task_save_data_t* sjob);
static int  s_task_save_write (struct s_task_save_data_t* sjob);
static void s_task_save_close (struct s_task_save_data_t* sjob);

/* --------------------------- PUBLISH HIST TASK --------------------------- */

/* ----------------------------- THE FULL LIST ----------------------------- */

#define NUM_TASKS 1
static task_t tasks[] = {
	{ // SAVE TO FILE 
		.client_handler = s_task_save_req_hn,
		.pkt_handler    = s_task_save_pkt_hn,
		.task_init_fn   = s_task_save_init,
		.task_fin_fn    = s_task_save_fin,
		.front_type     = ZMQ_REP,
		.front_addr     = "tcp://*:55555",
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
	// TO DO in data constructor
	// assert (sizeof (struct sjob_stats_t) == FDATA_OFF);
	assert (rxring != NULL);
	assert (NUM_TASKS == sizeof (tasks)/sizeof (task_t));
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
	assert (loop != NULL);
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
	assert (loop != NULL);
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
	assert (self_ != NULL);

	task_t* self = (task_t*) self_;
	assert (self->busy == 0);
	
	// int sig = zsock_wait (reader);
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
	assert (sig >= 0);
	if (sig == SIG_STOP)
	{
		s_msg (0, LOG_DEBUG, self->id,
			"Coordinator thread is terminating us");
		return -1;
	}
	assert (sig == SIG_WAKEUP);
	if (self->active == 0)
		return 0;

	self->busy = 1;
	/* Process packets */
	do
	{
		/* TO DO: is it faster if ifring_buf does no checks and instead
		 * we check if head == tail at the end */
		fpga_pkt* pkt = (fpga_pkt*) ifring_buf (
			self->rxring, self->head);

		uint16_t blen = ifring_len (self->rxring, self->head);
		uint16_t plen = pkt_len (pkt);
		if (blen != plen)
		{
			s_msgf (0, LOG_WARNING, self->id,
				"Packet length mismatch: header says %hu, "
				"ring slot is %hu", plen, blen);
			if (plen > blen)
				plen = blen;
		}
		int rc;
		/* TO DO: check packet */

		rc = self->pkt_handler (loop, pkt, plen, self);
		self->head = ifring_following (self->rxring, self->head);

		if (rc)
			break;

	} while (self->head != ifring_tail (self->rxring));

	if (self->error)
		self->active = 0;

	self->busy = 0;
	return self->error;
}

/*
 * Registered with the coordinator's loop. Receives SIG_DIED sent by a task and
 * terminates the coordinator's loop. 
 */
static int
s_die_hn (zloop_t* loop, zsock_t* reader, void* ignored)
{
	// int sig = zsock_wait (reader);
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
	assert (sig >= 0);

	if (sig == SIG_DIED)
	{
		s_msg (0, LOG_DEBUG, 0, "Task thread encountered an error");
		return -1;
	}
	assert (0); // we only deal with SIG_DIED 
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
	assert (self->data == NULL);
	assert (self->rxring != NULL);
	assert (self->frontend == NULL);
	assert (self->id > 0);
	assert (self->head == 0);
	assert (self->cur_frame == 0);
	assert (self->error == 0);
	assert (self->busy == 0);
	assert (self->active == 0);
	
	zloop_t* loop = zloop_new ();
	/* Only the coordinator thread should get interrupted, we wait for
	 * SIG_STOP. */
	zloop_set_nonstop (loop, 1);

	// s_msg (0, LOG_DEBUG, self->id, "Simulating error");
	// self->error = 1;
	// goto cleanup;

	/* Register the readers */
	if (self->front_addr || self->client_handler)
	{
		/* Did we forget to set one? */
		assert (self->client_handler != NULL);
		assert (self->front_addr != NULL);

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
		rc = zloop_reader (loop, pipe, s_sig_hn, self);
	}
	rc |= zloop_reader (loop, self->frontend, self->client_handler, self);
	if (rc)
	{
		s_msg (errno, LOG_ERR, self->id,
			"Could not register the zloop readers");
		self->error = 1;
		goto cleanup;
	}

	/* Call initializer */
	if (self->task_init_fn)
	{
		rc = self->task_init_fn (self);
		if (rc)
		{
			s_msg (errno, LOG_ERR, self->id,
				"Could not initialize thread data");
			self->error = 1;
			goto cleanup;
		}
	}

	zsock_signal (pipe, SIG_INIT); /* task_new will wait for this */
	s_msg (0, LOG_DEBUG, self->id, "Polling");
	rc = zloop_start (loop);
	assert (rc == -1); /* we don't get interrupted */

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

	if (self->task_fin_fn)
	{
		rc = self->task_fin_fn (self);
		if (rc)
		{
			s_msg (errno, LOG_ERR, self->id,
				"Could not cleanup thread data");
		}
		assert (self->data == NULL);
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
	assert (self != NULL);
	assert (rxring != NULL);

	self->rxring = rxring;
	/* TO DO: set the head */

	/* Start the thread, will block until the handler signals */
	self->shim = zactor_new (s_task_shim, self);
	assert (self->shim);
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
	assert (self_ != NULL);

	task_t* self = (task_t*) self_;
	assert (self->busy == 0);
	assert (self->active == 0);
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
		zsock_send (reader, REP_PIC, REQ_FAIL, 0, 0, 0, 0);
		s_task_save_close (sjob);
		return 0;
	}
	s_msgf (0, LOG_INFO, self->id, "Opened file %s for %s",
		sjob->filename, (fmode & O_RDWR ) ? "writing" : "reading" );

	if (sjob->max_ticks == 0)
	{ /* just read in stats and send reply */
		s_task_save_read (sjob);
		zsock_send (reader, REP_PIC, REQ_OK, 
			sjob->st.ticks,
			sjob->st.size,
			sjob->st.frames,
			sjob->st.frames_lost);
		s_task_save_close (sjob);
		return 0;
	}

	/* Disable polling on the reader until the job is done */
	zloop_reader_end (loop, reader);
	self->head = ifring_head (self->rxring);
	self->active = 1;
	return 0;
}

/*
 * Saves packets to a file.
 */
static int
s_task_save_pkt_hn (zloop_t* loop, fpga_pkt* pkt, uint16_t len, task_t* self)
{
	struct s_task_save_data_t* sjob =
		(struct s_task_save_data_t*) self->data;
	assert (sjob->filename != NULL);
	assert (sjob->fd != -1);
	assert (self->active);

	/* TO DO: how to handle errors */
	int rc = write (sjob->fd, pkt, len);
	if (rc == -1)
	{
		s_msgf (errno, LOG_ERR, self->id,
			"Could not write to file [%d]", sjob->fd);
		self->error = 1;
		return -1;
	}

	uint16_t prev_frame = self->cur_frame;
	self->cur_frame = frame_seq (pkt);

	/* TO DO: save err flags */
	if (is_tick (pkt))
		sjob->st.ticks++;
	sjob->st.size += len;
	sjob->st.frames++;
	sjob->st.frames_lost += (uint64_t) (
			(uint16_t)(self->cur_frame - prev_frame) - 1);

	if (sjob->st.ticks == sjob->max_ticks)
	{
		self->active = 0;
		s_msgf (0, LOG_INFO, self->id,
			"Finished writing %lu ticks to file %s",
			sjob->st.ticks, sjob->filename);
		/* Send message to client */
		/* TO DO: check rc */
		zsock_send (self->frontend, REP_PIC, REQ_OK, 
			sjob->st.ticks,
			sjob->st.size,
			sjob->st.frames,
			sjob->st.frames_lost);
		s_task_save_write (sjob);
		s_task_save_close (sjob);
		/* Enable polling on the reader */
		rc = zloop_reader (loop, self->frontend,
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

/*
 * data is a struct s_task_save_data_t.
 */
static int
s_task_save_init (task_t* self)
{
	assert (self != NULL);
	self->data = malloc (sizeof (struct s_task_save_data_t));
	if (self->data == NULL)
	{
		s_msg (errno, LOG_ERR, self->id, "Cannot allocate data");
		return -1;
	}
	memset (self->data, 0, sizeof (struct s_task_save_data_t));

	struct s_task_save_data_t* sjob =
		(struct s_task_save_data_t*) self->data;
	sjob->fd = -1;
	return 0;
}
static int
s_task_save_fin (task_t* self)
{
	assert (self != NULL);
	assert (self->active == 0);
	struct s_task_save_data_t* sjob =
		(struct s_task_save_data_t*) self->data;
	assert (sjob != NULL);
	if (sjob->fd != -1)
	{
		assert (sjob->filename != NULL);
		zsock_send (self->frontend, REP_PIC, REQ_OK, 
			sjob->st.ticks,
			sjob->st.size,
			sjob->st.frames,
			sjob->st.frames_lost);
		s_task_save_write (sjob);
		s_task_save_close (sjob);
	}

	free (self->data);
	self->data = NULL;
	return 0;
}

/*
 * Opens the file and sets max_ticks. Returns 0 on success, -1 on error.
 */
static int
s_task_save_open (struct s_task_save_data_t* sjob, mode_t fmode)
{
	assert (sjob != NULL);
	assert (sjob->filename != NULL);
	assert (sjob->fd == -1);
	assert (sjob->st.ticks == 0);
	assert (sjob->st.size == 0);
	assert (sjob->st.frames == 0);
	assert (sjob->st.frames_lost == 0);
	assert (sjob->st.frames_dropped == 0);
	assert (sjob->st.errors == 0);

	/* Open the file */
	sjob->fd = open (sjob->filename, fmode,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (sjob->fd == -1)
		return -1;

	int rc;
	rc = lseek (sjob->fd, FDATA_OFF, 0);
	if (rc == (off_t)-1 || rc < FDATA_OFF)
		return -1;

	return 0;
}

/*
 * Reads stats previously saved to file. Used when client requests a status for
 * filename.
 */
static int
s_task_save_read (struct s_task_save_data_t* sjob)
{
	assert (sjob != NULL);
	assert (sjob->filename != NULL);
	assert (sjob->fd != -1);
	assert (sjob->st.ticks == 0);
	assert (sjob->st.size == 0);
	assert (sjob->st.frames == 0);
	assert (sjob->st.frames_lost == 0);
	assert (sjob->st.frames_dropped == 0);
	assert (sjob->st.errors == 0);

	int rc = lseek (sjob->fd, 0, 0);
	if (rc)
		return rc;
	
	rc = read (sjob->fd, &sjob->st, FDATA_OFF);
	if (rc < FDATA_OFF)
		return rc;
	
	return 0;
}

/*
 * Writes stats to a currently open file. Used right before closing it.
 */
static int
s_task_save_write (struct s_task_save_data_t* sjob)
{
	assert (sjob != NULL);
	assert (sjob->filename != NULL);
	assert (sjob->fd != -1);

	int rc = lseek (sjob->fd, 0, 0);
	if (rc)
		return rc;
	
	rc = write (sjob->fd, &sjob->st, FDATA_OFF);
	if (rc < FDATA_OFF)
		return rc;
	
	return 0;
}

/*
 * Closes the file descriptor, nullifies and resets stats.
 */
static void
s_task_save_close (struct s_task_save_data_t* sjob)
{
	assert (sjob != NULL);

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
	sjob->st.frames_dropped = 0;
	sjob->st.errors = 0;
}

/* ------------------------------------------------------------------------- */
/* --------------------------- PUBLISH HIST TASK --------------------------- */
/* ------------------------------------------------------------------------- */
