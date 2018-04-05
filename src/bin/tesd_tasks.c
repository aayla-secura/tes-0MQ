/*
 * -----------------------------------------------------------------
 * --------------------------- DEV NOTES ---------------------------
 * -----------------------------------------------------------------
 * There is a separate thread for each "task". Threads are zactors.
 * They are started by tasks_start (defined here, declared in
 * tesd_tasks_coordinator.h), which should be called by the
 * coordinator.
 *
 * Tasks are defined in a static global array, see THE TASK LIST.
 *
 * Tasks are largely similar, so we pass the same handler,
 * s_task_shim, to zactor_new. It is responsible for doing most of
 * the work. Tasks are described by a struct _task_t (see
 * tesd_tasks.h).
 *
 * Tasks have read-only access to rings (they cannot modify the
 * cursor or head) and each task keeps its own head (for each ring),
 * which is visible by the coordinator (tesd.c). For each ring, the
 * coordinator sets the true head to the per-task head which lags
 * behind all others.
 *
 * s_task_shim registers a generic reader, s_sig_hn, for handling
 * the signals from the coordinator. Upon SIG_STOP s_sig_hn exits,
 * upon SIG_WAKEUP it calls calls the task's specific packet handler
 * for each packet in each ring. It keeps track of the previous
 * frame and protocol sequences (the task's packet handler can make
 * use of those as well, e.g. to track lost frames). For convenience
 * the number of missed frames (difference between previous and
 * current frame sequences mod 2^16) is passed to the pkt_handler.
 * s_sig_hn also takes care of updating the task's head.
 *
 * If the task defines public endpoint addresses, s_task_shim will
 * open the socket, and if the endpoint defines a handler, it will
 * register it with the task's loop. Each task has a pointer for its
 * own data.
 *
 * Before entering the loop, s_task_shim will call the task
 * initializer, if it is set. So it can allocate the pointer to its
 * data and do anything else it wishes (talk to clients, etc).
 *
 * Tasks which set their autoactivate flag are activated before
 * entering the loop. Otherwise the task should activate itself from
 * within its initializer or in its endpoint handlers.
 *
 * Each endpoint defined with the automute flag will have its handler
 * deregistered from the loop upon task activation, and registered
 * again upon deactivation. Useful for tasks which deal with one
 * client at a time, such as REQ/REP tasks.
 *
 * FRONTEND-TYPE SPECIFICS:
 *   If any of the endpoints is an XPUB and is defined with the
 *   automanage flag, a generic handler is registered for it. It
 *   inspects messages received, updates the list of active
 *   subscription patterns ((struct _task_endpoint_t*)->pub.subscriptions).
 *   If the endpoint also has the autosleep flag it will be
 *   deactivated when the socket has no subscribers and reactivated at
 *   the first subscription.
 *   Note that any additional handler set for the socket will also be
 *   registered in which case it must not try to receive the message
 *   from the socket. It may inspect the list of subscriptions since
 *   it will be called after the default handler. Subscriptions are
 *   appended to list, so the last one is at the tail.
 *   For XPUB endpoints (struct _task_endpoint_t*)->pub.subscriptions
 *   is initialized to a new zlistx_t regardless of the automanaged
 *   flag, so the socket handler can use it straight away. Furthermore
 *   the comparator function is set to (a wrapper around) strcmp, the
 *   duplicator function to (a wrapper around) strdup and the
 *   destructor to (a wrapper) around free. Tasks may change the
 *   comparator/duplicator/destructor in their data_init method.
 *
 *   If any of the endpoints is an XSUB or SUB it can make use of
 *   endp_subscribe and endp_unsubscribe to subscribe and unsubscribe (the
 *   type XSUB vs SUB is checked and the appropriate action is taken.
 *   The no. of subscriptions (struct _task_endpoint_t*)->pub.nsubs
 *   and the list of (struct _task_endpoint_t*)->pub.subscriptions is
 *   updated. The same default comparator/duplicator/destructor is set
 *   as for XPUB.
 *   If the endpoint also has the autosleep flag it will be
 *   deactivated when the socket has no subscriptions and reactivated
 *   when it requests a new subscription.
 *   The automanaged flag is not used.
 *
 * Right after the loop terminates, s_task_shim will call the task
 * finalizer, so it can cleanup its data and possibly send final
 * messages to clients.
 *
 * The actual task is done inside the endpoint handlers and pkt_handler.
 *
 *   each endpoint handler processes messages on the public socket. If
 *   no endpoint is set, the task has no public interface.
 *
 *   pkt_handler is called (by s_sig_hn when receiving SIG_WAKEUP) for
 *   each packet in each ring.
 *
 * Tasks have access to their zloop so they can enable or disable
 * readers (e.g. a endpoint handler can disable itself after
 * receiving a job and the pkt_handler can re-enable it when done).
 *
 * If either handler encounters a fatal error, it must return with
 * TASK_ERROR. The server is stopped cleanly in such case.
 *
 * If the task wants to deactivate itself, it should call
 * task_deactivate. Alternatively it can return with TASK_SLEEP
 * from within the pkt_handler. The task then won't be receiving
 * SIG_WAKEUP and its heads won't be synchronized with the real
 * heads.
 *
 * After talking to a client, if it needs to process packets again,
 * the task must reactivate via task_activate. Note that tasks
 * which do not talk to clients have no way of reactivating
 * themselves, so their pkt_handler should never return with
 * TASK_SLEEP.
 *
 * The error, busy and active flags are handled by s_sig_hn and
 * s_task_shim. Tasks' handlers should generally only make use of
 * task_activate, task_deactivate and return codes (0,
 * TASK_SLEEP or TASK_ERROR).
 *
 * Note on zactor:
 * We start the task threads using zactor high-level class, which on
 * UNIX systems is a wrapper around pthread_create. zactor_new
 * creates two PAIR zmq sockets and creates a detached thread
 * caliing a wrapper (zactor.c:s_thread_shim) around the hanlder of
 * our choice. It starts the actual handler (which we pass to
 * zactor_new), passing it its end of the pipe (a PAIR socket) as
 * well as a void* argument of our choice (again, given to
 * zactor_new). The handler must signal down the pipe using
 * zsock_signal (doesn't matter the byte status), since zactor_new
 * will be waiting for this before it returns. The handler must
 * listen on the pipe for a terminating signal, which is sent by the
 * actor's destructor (called by zactor_destroy). Upon receiving
 * this signal the handler must return. It returns into
 * s_thread_shim which signals down the pipe before destroying that
 * end of the pipe. The destructor must wait for this signal before
 * returning into zactor_destroy, which destroys the other end of
 * the pipe and returns to the caller. Hence zactor_destroy acts
 * analogously to pthread_cancel + pthread_join (for joinable
 * threads). The default destructor sends a single-frame message
 * from the string "$TERM". zactor_set_destructor, which can set
 * a custom destructor is a DRAFT method only available in latest
 * versions, so we stick to the default one for now. But since we
 * want to deal with integer signals, and not string messages, we
 * define s_task_stop as a wrapper around zactor_destroy, which
 * sends SIG_STOP and then calls zactor_destroy to wait for the
 * handler to return.
 *
 * -----------------------------------------------------------------
 ------------------------------- TO DO -----------------------------
 * -----------------------------------------------------------------
 * - Alert subscribers to PUB tasks when shutting down.
 * - Test with using more than one of the rings: how to get the NIC
 *   to fill the rest.
 * - Why does writing to file fail with "unhandled syscall" when
 *   running under valgrind? A:
 *   https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=219715
 * - Print debugging stats every UPDATE_INTERVAL via the
 *   coordinator.
 * - Config file shared between tasks, i.e. global config that can be
 *   changed or queried by either the coordinator or a dedicated task.
 */

#include "tesd_tasks.h"
#include "tesd_tasks_coordinator.h"

static const char* s_config_dir; // stored and given by coordinator

static zactor_fn       s_task_shim;
static zloop_reader_fn s_sig_hn;
static zloop_reader_fn s_die_hn;
static zloop_reader_fn s_sub_hn;
static zlistx_comparator_fn s_item_cmp;
static zlistx_duplicator_fn s_item_dup;
static zlistx_destructor_fn s_item_free;

static int  s_task_start (tes_ifdesc* ifd, task_t* self);
static void s_task_stop (task_t* self);
static int  s_task_next_ring (task_t* self, uint16_t* missed_p);
static int  s_task_dispatch (task_t* self, zloop_t* loop,
	uint16_t ring_id, uint16_t missed);
static int s_endp_sub_send (task_endp_t* endpoint, char cmd,
	char* pattern);
static void s_endp_sub_add (task_endp_t* endpoint, char* pattern);
static void s_endp_sub_del (task_endp_t* endpoint, char* pattern);

/* ------------------------ THE TASK LIST ----------------------- */

#define NUM_TASKS 6
static task_t s_tasks[] = {
	{ /* PACKET INFO */
		.pkt_handler = task_info_pkt_hn,
		.data_init   = task_info_init,
		.endpoints   = {
			{
				.handler   = task_info_req_hn,
				.addresses = "tcp://*:" TES_INFO_LPORT,
				.type      = ZMQ_REP,
				.automute  = true,
			},
		},
		.color       = ANSI_FG_YELLOW,
	},
	{ /* CAPTURE */
		.pkt_handler = task_cap_pkt_hn,
		.data_init   = task_cap_init,
		.data_fin    = task_cap_fin,
		.endpoints   = {
			{
				.handler   = task_cap_req_hn,
				.addresses = "tcp://*:" TES_CAP_LPORT,
				.type      = ZMQ_REP,
				.automute  = true,
			},
		},
		.color       = ANSI_FG_BLUE,
	},
	{ /* GET AVG TRACE */
		.pkt_handler = task_avgtr_pkt_hn,
		.data_init   = task_avgtr_init,
		.endpoints   = {
			{
				.handler   = task_avgtr_req_hn,
				.addresses = "tcp://*:" TES_AVGTR_LPORT,
				.type      = ZMQ_REP,
				.automute  = true,
			},
		},
		.color       = ANSI_FG_GREEN,
	},
	{ /* PUBLISH MCA HIST */
		.pkt_handler = task_hist_pkt_hn,
		.data_init   = task_hist_init,
		.data_wakeup = task_hist_wakeup,
		.endpoints   = {
			{
				.addresses = "tcp://*:" TES_HIST_LPORT,
				.type      = ZMQ_XPUB,
				.pub.autosleep  = true,
				.pub.automanage = true,
			},
		},
		.color       = ANSI_FG_CYAN,
	},
	{ /* PUBLISH JITTER HIST */
		.pkt_handler = task_jitter_pkt_hn,
		.data_init   = task_jitter_init,
		.data_wakeup = task_jitter_wakeup,
		.endpoints   = {
			{
				.handler   = task_jitter_req_hn,
				.addresses = "tcp://*:" TES_JITTER_REP_LPORT,
				.type      = ZMQ_REP,
			},
			{
				.addresses = "tcp://*:" TES_JITTER_PUB_LPORT,
				.type      = ZMQ_XPUB,
				.pub.autosleep  = true,
				.pub.automanage = true,
			},
		},
		.color       = ANSI_FG_MAGENTA,
	},
	{ /* RAW COINCIDENCE */
		.pkt_handler = task_coinc_pkt_hn,
		.data_init   = task_coinc_init,
		.data_wakeup = task_coinc_wakeup,
		.endpoints   = {
			{
				.handler   = task_coinc_req_hn,
				.addresses = "tcp://*:" TES_COINC_REP_LPORT,
				.type      = ZMQ_REP,
			},
			{
				.handler   = task_coinc_req_th_hn,
				.addresses = "tcp://*:" TES_COINC_REP_TH_LPORT,
				.type      = ZMQ_REP,
			},
			{
				.addresses = "tcp://*:" TES_COINC_PUB_LPORT,
				.type      = ZMQ_XPUB,
				.pub.autosleep  = true,
				.pub.automanage = true,
			},
		},
		.color       = ANSI_FG_YELLOW,
	},
};

/* -------------------------------------------------------------- */
/* ----------------------- COORDINATOR API ---------------------- */
/* -------------------------------------------------------------- */

int
tasks_start (tes_ifdesc* ifd, zloop_t* c_loop, const char* confdir)
{
	assert (ifd != NULL);
	assert (sizeof (s_tasks) == NUM_TASKS * sizeof (task_t));
	
	s_config_dir = confdir;
	
	int rc;
	for (int t = 0; t < NUM_TASKS; t++)
	{
		s_tasks[t].id = t + 1;
		logmsg (0, LOG_DEBUG, "Starting task #%d", t + 1);
		rc = s_task_start (ifd, &s_tasks[t]);
		if (rc != 0)
		{
			logmsg (errno, LOG_ERR,
				"Could not start tasks");
			return -1;
		}
	}

	if (c_loop != NULL)
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
		logmsg (0, LOG_DEBUG, "Registering reader for task #%d", t);
		task_t* self = &s_tasks[t];
		rc = zloop_reader (loop, zactor_sock(self->shim),
			s_die_hn, NULL);
		if (rc == -1)
		{
			logmsg (errno, LOG_ERR,
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
		logmsg (0, LOG_DEBUG,
			"Unregistering reader for task #%d", t);
		task_t* self = &s_tasks[t];
		zloop_reader_end (loop, zactor_sock(self->shim));
	}
}

int
tasks_wakeup (void)
{
	for (int t = 0; t < NUM_TASKS; t++)
	{
		task_t* self = &s_tasks[t];
		if (self->active && ! self->busy)
		{
			int rc = zsock_signal (self->shim, SIG_WAKEUP);
			if (rc == -1)
			{
				logmsg (errno, LOG_ERR,
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
		logmsg (0, LOG_DEBUG, "Stopping task #%d", t);
		task_t* self = &s_tasks[t];
		s_task_stop (self);
	}
}

uint32_t*
tasks_get_heads (void)
{
	/* Use a static storage for the returned array. */
	static uint32_t heads[NUM_RINGS];

	bool updated = false; /* set to 1 if at least one active task */
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
					tes_ifring* rxring =
						tes_if_rxring (self->ifd, r);
					heads[r] = tes_ifring_earlier_id (
							rxring, heads[r],
							self->heads[r]);
				}
			}
			else
			{
				for (int r = 0; r < NUM_RINGS; r++)
					heads[r] = self->heads[r];
				updated = true;
			}
		}
	}
	return (updated ? heads : NULL);
}

/* -------------------------------------------------------------- */
/* -------------------------- TASKS API ------------------------- */
/* -------------------------------------------------------------- */

int
task_activate (task_t* self)
{
	assert (self != NULL);
	assert (self->pkt_handler != NULL);

	if (self->data_wakeup != NULL)
	{
		int rc = self->data_wakeup (self);
		if (rc != 0)
		{
			logmsg (errno, LOG_ERR,
				"Could not prepare thread data on activation");
			return TASK_ERROR;
		}
	}

	for (task_endp_t* endpoint = &self->endpoints[0];
			endpoint->addresses != NULL &&
			endpoint <= &self->endpoints[MAX_FRONTENDS - 1];
			endpoint++)
	{
		if (endpoint->automute)
			zloop_reader_end (self->loop, endpoint->sock);
	}

	for (int r = 0; r < NUM_RINGS; r++)
	{
		tes_ifring* rxring = tes_if_rxring (self->ifd, r);
		self->heads[r] = tes_ifring_head (rxring);
	}

	self->active = true;
	self->just_activated = true;

	return 0;
}

int
task_deactivate (task_t* self)
{
	assert (self != NULL);

	if (self->data_sleep != NULL)
	{
		int rc = self->data_sleep (self);
		if (rc != 0)
		{
			logmsg (errno, LOG_ERR,
				"Could not prepare thread data on deactivation");
			return TASK_ERROR;
		}
	}

	for (task_endp_t* endpoint = &self->endpoints[0];
			endpoint->addresses != NULL &&
			endpoint <= &self->endpoints[MAX_FRONTENDS - 1];
			endpoint++)
	{
		if ( ! endpoint->automute )
			continue;
		
		dbg_assert (endpoint->handler != NULL);
		int rc = zloop_reader (self->loop, endpoint->sock,
				endpoint->handler, self);
		if (rc == -1)
		{
			logmsg (errno, LOG_ERR,
					"Could not re-enable the zloop reader");
			return TASK_ERROR;
		}
	}

	self->active = false;

	return 0;
}

ssize_t
task_conf (task_t* self, void* conf, size_t len, int cmd)
{
	assert (self != NULL);
	assert (cmd == TES_TASK_SAVE_CONF || cmd == TES_TASK_READ_CONF);
	if (s_config_dir == NULL)
		return 0;

	char conffile[PATH_MAX];
	snprintf (conffile, PATH_MAX, "%s/task_%d.bin",
		s_config_dir, self->id);
	int flags = 0;
	
	if (cmd == TES_TASK_SAVE_CONF)
		flags = O_CREAT | O_TRUNC | O_WRONLY;
	else
		flags = O_RDONLY;

	int fd = open (conffile, flags, S_IRUSR | S_IWUSR);
	if (fd == -1)
	{
		logmsg (errno, LOG_WARNING,
			"Could not open config file '%s'", conffile);
		return -1;
	}

	if (cmd == TES_TASK_SAVE_CONF)
		return write (fd, conf, len);
	return read (fd, conf, len);
}

int
endp_subscribe (task_endp_t* endpoint, char* pattern)
{
	assert (pattern != NULL);
	assert (endpoint != NULL);
	
	if (s_endp_sub_send (endpoint, 1, pattern) == TASK_ERROR)
		return TASK_ERROR;

	s_endp_sub_add (endpoint, pattern);
	return 0;
}

int
endp_unsubscribe (task_endp_t* endpoint, char* pattern)
{
	assert (pattern != NULL);
	assert (endpoint != NULL);
	
	if (s_endp_sub_send (endpoint, 0, pattern) == TASK_ERROR)
		return TASK_ERROR;
	
	s_endp_sub_del (endpoint, pattern);
	return 0;
}

/* -------------------------------------------------------------- */
/* -------------------------- INTERNAL -------------------------- */
/* -------------------------------------------------------------- */

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
	assert (self->ifd != NULL);
	assert (self->id > 0);

	/* Set log prefix. */
	char log_id[32] = {0};
	if (ami_daemon())
		snprintf (log_id, sizeof (log_id), "[Task #%d]     ", self->id);
	else if (self->color != NULL)
		snprintf (log_id, sizeof (log_id), "%s[Task #%d]%s     ",
			self->color, self->id, ANSI_RESET);
	set_logid (log_id);

	/* Set CPU affinity. */
	if (pth_set_cpuaff (self->id) == -1)
		logmsg (errno, LOG_WARNING, "Cannot set cpu affinity");
	
	/* Block signals in each tasks's thread. */
	struct sigaction sa = {0};
	sigfillset (&sa.sa_mask);
	pthread_sigmask (SIG_BLOCK, &sa.sa_mask, NULL);
	
	zloop_t* loop = zloop_new ();
	self->loop = loop;
	/* Only the coordinator thread should get interrupted, we wait
	 * for SIG_STOP. */
#if (CZMQ_VERSION_MAJOR > 3)
	zloop_set_nonstop (loop, 1);
#else
	zloop_ignore_interrupts (loop);
#endif

	// logmsg (0, LOG_DEBUG, "Simulating error");
	// self->error = true;
	// goto cleanup;

	/* Open and bind the public interfaces */
	for (task_endp_t* endpoint = &self->endpoints[0];
			endpoint->addresses != NULL &&
			endpoint <= &self->endpoints[MAX_FRONTENDS - 1];
			endpoint++)
	{
		endpoint->sock = zsock_new (endpoint->type);
		if (endpoint->sock == NULL)
		{
			logmsg (errno, LOG_ERR,
				"Could not open the public interfaces");
			self->error = true;
			goto cleanup;
		}
		rc = zsock_attach (endpoint->sock, endpoint->addresses, 1);
		if (rc == -1)
		{
			logmsg (errno, LOG_ERR,
				"Could not bind the public interfaces");
			self->error = true;
			goto cleanup;
		}
		logmsg (0, LOG_INFO,
			"Listening on port(s) %s", endpoint->addresses);

		if (endpoint->type == ZMQ_XPUB ||
			endpoint->type == ZMQ_XSUB || endpoint->type == ZMQ_SUB)
		{
			endpoint->pub.subscriptions = zlistx_new ();
			zlistx_set_comparator (endpoint->pub.subscriptions,
				s_item_cmp);
			zlistx_set_duplicator (endpoint->pub.subscriptions,
				s_item_dup);
			zlistx_set_destructor (endpoint->pub.subscriptions,
				s_item_free);
		}

		rc = 0;
		if (endpoint->pub.automanage)
		{ /* default XPUB handler */
			assert (endpoint->type == ZMQ_XPUB);
			rc = zloop_reader (loop, endpoint->sock, s_sub_hn, self);
		}
		if (rc == 0 && endpoint->handler != NULL)
		{ /* task's own handler */
			rc = zloop_reader (loop, endpoint->sock,
				endpoint->handler, self);
		}
		if (rc == -1)
		{
			logmsg (errno, LOG_ERR,
				"Could not register the zloop endpoint readers");
			self->error = true;
			goto cleanup;
		}
	}

	/* Register the coordinator pipe. */
	rc = zloop_reader (loop, pipe, s_sig_hn, self);
	if (rc == -1)
	{
		logmsg (errno, LOG_ERR,
			"Could not register the zloop PAIR reader");
		self->error = true;
		goto cleanup;
	}

	/* Call initializer */
	if (self->data_init != NULL)
	{
		rc = self->data_init (self);
		if (rc != 0)
		{
			logmsg (errno, LOG_ERR,
				"Could not initialize thread data");
			self->error = true;
			goto cleanup;
		}
	}

	logmsg (0, LOG_DEBUG, "Polling");
	zsock_signal (pipe, SIG_INIT); /* task_new will wait for this */
	
	if (self->autoactivate)
	{
		rc = task_activate (self);
		if (rc == TASK_ERROR)
		{
			logmsg (errno, LOG_ERR,
				"Could not autoactivate task");
			self->error = true;
			goto cleanup;
		}
		dbg_assert (rc == 0);
	}
	
	rc = zloop_start (loop);
	dbg_assert (rc == -1); /* we don't get interrupted */

cleanup:
	/*
	 * zactor_destroy waits for a signal from s_thread_shim (see DEV
	 * NOTES). To avoid returning from zactor_destroy prematurely,
	 * we only send SIG_DIED if we exited due to an error on our
	 * part (in one of the handlers).
	 */
	if (self->error)
		zsock_signal (pipe, SIG_DIED);

	if (self->data_fin != NULL)
	{
		rc = self->data_fin (self);
		if (rc != 0)
		{
			logmsg (errno, LOG_ERR,
				"Could not cleanup thread data");
		}
		dbg_assert (self->data == NULL);
	}
	zloop_destroy (&loop);
	for (task_endp_t* endpoint = &self->endpoints[0];
			endpoint->addresses != NULL &&
			endpoint <= &self->endpoints[MAX_FRONTENDS - 1];
			endpoint++)
		zsock_destroy (&endpoint->sock);
	logmsg (0, LOG_DEBUG, "Done");

	if (self->pkt_handler == NULL)
		return;

#if DEBUG_LEVEL >= VERBOSE
	logmsg (0, LOG_DEBUG,
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
		logmsg (0, LOG_DEBUG,
			"Ring %d received: %lu", r,
			self->dbg_stats.pkts.rcvd_in[r]);
#endif
}

/*
 * Registered with each task's loop. Receives signals sent on behalf
 * of the coordinator (via tasks_wakeup or tasks_stop). On
 * SIG_WAKEUP calls the task's packet handler. On SIG_STOP
 * terminates the task's loop.
 */
static int
s_sig_hn (zloop_t* loop, zsock_t* reader, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;
	dbg_assert ( ! self->busy );
	self->busy = true;
	
	int sig = zsock_wait (reader);
	dbg_assert (sig != -1); /* we don't get interrupted */
	
	if (sig == SIG_STOP)
	{
		logmsg (0, LOG_DEBUG,
			"Coordinator thread is terminating us");
		return -1;
	}
	dbg_assert (sig == SIG_WAKEUP);

	/* We should never have received a WAKEUP if we are not active,
	 * but we do, after a job is done. Signals must be queueing in
	 * the PAIR socket and coming with a slight delay. */
	if ( ! self->active )
	{
#if DEBUG_LEVEL >= VERBOSE
		if (self->dbg_stats.wakeups_inactive == 0)
			logmsg (0, LOG_DEBUG,
				"First inactive wakeup");
		self->dbg_stats.wakeups_inactive++;
#endif
		self->busy = false;
		return 0;
	}
#if DEBUG_LEVEL >= VERBOSE
	self->dbg_stats.wakeups++;
#endif
	// self->busy = true;

	/* Process packets. */
#if DEBUG_LEVEL >= VERBOSE
	bool first = true;
#endif
	while (true)
	{
		uint16_t missed;   /* will hold the jump in frame seq */
		int next_ring_id = s_task_next_ring (self, &missed);

		/*
		 * We should never have received a WAKEUP if there are no
		 * new packets, but sometimes we do, why?
		 */
		if (next_ring_id < 0)
		{
#if DEBUG_LEVEL >= VERBOSE
			if (first)
				self->dbg_stats.wakeups_false++;
#endif
			break;
		}
#if DEBUG_LEVEL >= VERBOSE
		first = false;
#endif

		int rc = s_task_dispatch (self, loop, next_ring_id, missed);
		/* In case packet hanlder or dispatcher need to know that
		 * it's the first time after activation. */
		self->just_activated = false;

		if (rc == TASK_SLEEP)
		{
			rc = task_deactivate (self);
			break;
		}

		if (rc == TASK_ERROR)
		{ /* either pkt_handler or task_deactivate failed */
			self->error = true;
			break;
		}
	}

	self->busy = false;
	return (self->error ? -1 : 0);
}

/*
 * Registered with the coordinator's loop. Receives SIG_DIED sent by
 * a task and terminates the coordinator's loop.
 */
static int
s_die_hn (zloop_t* loop, zsock_t* reader, void* ignored)
{
	dbg_assert (ignored == NULL);

	int sig = zsock_wait (reader);
	// if (sig == -1)
	// { /* this shouldn't happen */
	//   logmsg (0, LOG_DEBUG, "Receive interrupted");
	//   return -1;
	// }

	if (sig == SIG_DIED)
	{
		logmsg (0, LOG_DEBUG,
			"Task thread encountered an error");
		return -1;
	}
	assert (false); /* we only deal with SIG_DIED  */
}

/*
 * Registered with a task's XPUB endpoint (if automanage is set).
 * If autosleep is true, will deactivate task on last unsubscription
 * and activate it on first subscription.
 *
 * XPUB will receive a message of the form "\x01<prefix>" the first
 * time a client subscribes to the port with a prefix <prefix>, and
 * will receive a message of the form "\x00<prefix>" when the last
 * client subscribed to <prefix> unsubscribes.
 * It will also receive any message sent to the port (by an
 * ill-behaved client) that does not begin with "\x00" or "\x01",
 * these should be ignored.
 */
int
s_sub_hn (zloop_t* loop, zsock_t* reader, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;

	zmsg_t* msg = zmsg_recv (reader);
	/* We don't get interrupted, this should not happen. */
	assert (msg != NULL);

	if (zmsg_size (msg) != 1)
	{
		logmsg (0, LOG_DEBUG,
			"Got a spurious %lu-frame message", zmsg_size(msg));
		zmsg_destroy (&msg);
		return 0;
	}

#if DEBUG_LEVEL >= VERBOSE
	zframe_t* f = zmsg_first (msg);
	char* hexstr = zframe_strhex (f);
	logmsg (0, LOG_DEBUG, "Got message %s", hexstr);
	zstr_free (&hexstr);
#endif

	/* Find the endpoint for which to update subscriber numbers. */
	task_endp_t* endpoint = NULL;
	for (endpoint = &self->endpoints[0];
			endpoint->sock != NULL && endpoint->sock != reader;
			endpoint++)
		;
	dbg_assert (endpoint->sock == reader); /* couldn't find reader? */

	char* msgstr = zmsg_popstr (msg);
	zmsg_destroy (&msg);
	char req_rc = msgstr[0];
	if (req_rc == 0)
		s_endp_sub_del (endpoint, msgstr + 1);
	else if (req_rc == 1)
		s_endp_sub_add (endpoint, msgstr + 1);
	else
	{
		logmsg (0, LOG_DEBUG, "Got a spurious message");
		zstr_free (&msgstr);
		return 0;
	}
	zstr_free (&msgstr);

	if ( ! endpoint->pub.autosleep )
		return 0;

	if (endpoint->pub.nsubs == 1)
	{
		logmsg (0, LOG_DEBUG,
			"First subscription, activating");
		/* Wakeup packet handler. */
		task_activate (self);
	}
	else if (endpoint->pub.nsubs == 0)
	{
		logmsg (0, LOG_DEBUG,
			"Last unsubscription, deactivating");
		/* Deactivate packet handler. */
		task_deactivate (self);
	}

	return 0;
}

static int
s_item_cmp (const void* itemA, const void* itemB)
{
	return strcmp ((const char*)itemA, (const char*)itemB);
}

static void*
s_item_dup (const void* item)
{
	return (void*)strdup ((const char*)item);
}

static void
s_item_free (void** item_p)
{
	assert (item_p != NULL);
	if (*item_p != NULL)
		free (*item_p);
	*item_p = NULL;
}

/*
 * Initializes a task_t and starts a new thread using zactor_new.
 * Registers the task's back end of the pipe with the coordinator's
 * loop.
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
	/* zactor_new does not check the signal, so no way to know if
	 * there was an error. As a workaroung the task thread will send
	 * a second signal when it is ready (or when it fails) and we
	 * wait for it here. */
	int rc = zsock_wait (self->shim);
	if (rc == SIG_DIED)
	{
		logmsg (0, LOG_DEBUG,
			"Task thread failed to initialize");
		return -1;
	}
	assert (rc == SIG_INIT);
	logmsg (0, LOG_DEBUG, "Task thread initialized");

	return 0;
}

/*
 * This is to be used instead of zactor_destroy, as a workaround for
 * not setting a custom destructor.
 */
static void
s_task_stop (task_t* self)
{
	assert (self != NULL);
	if (self->shim == NULL)
	{
		logmsg (0, LOG_DEBUG,
			"Task had already exited");
		return;
	}

	zsock_set_sndtimeo (self->shim, 0);
	/* Task will exit after this. */
	zsock_signal (self->shim, SIG_STOP);
	/* Wait for the final signal from zactor's s_thread_shim.
	 * zactor_destroy will send "$TERM" which will be ignored; not
	 * a problem. */
	zactor_destroy (&self->shim);
}

/*
 * Chooses the ring which contains the next packet to be inspected.
 */
static int
s_task_next_ring (task_t* self, uint16_t* missed_p)
{
	dbg_assert (self != NULL);

	int next_ring_id = -1; /* next to process */

	if (unlikely (self->just_activated))
	{
		/*
		 * If first time after activation, set the previous sequence
		 * and choose the ring by comparing the heads of all rings.
		 * Find the "smallest" frame sequence among the heads.
		 * Treat seq. no. A as ahead of seq. no. B if B - A
		 * is > UINT16_MAX/2.
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
		 * Otherwise, choose the ring based on prev_seq. Allowing
		 * for lost frames, simply take the ring for which the
		 * task's head packet is closest in sequence to the last
		 * seen frame sequence.
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
 * Loops over the given ring until either reaching the tail or
 * seeing a discontinuity in frame sequence. For each buffer calls
 * the task's pkt_handler.
 * Returns 0 if all packets until the tail are processed.
 * Returns TASK_SLEEP or TASK_ERROR if pkt_handler does so.
 * Returns ?? if a jump in frame sequence is seen (TO DO).
 */

static int
s_task_dispatch (task_t* self, zloop_t* loop,
		uint16_t ring_id, uint16_t missed)
{
	dbg_assert (self != NULL);
	dbg_assert (loop != NULL);

	tes_ifring* rxring = tes_if_rxring (self->ifd, ring_id);
	dbg_assert ( self->heads[ring_id] != tes_ifring_tail (rxring) );
#if DEBUG_LEVEL >= VERBOSE
	self->dbg_stats.rings_dispatched++;
#if DEBUG_LEVEL >= LETS_GET_NUTS
	if (missed)
	{
		tespkt* pkt = (tespkt*) tes_ifring_buf (
			rxring, self->heads[ring_id]);
		logmsg (0, LOG_DEBUG,
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
#if DEBUG_LEVEL >= VERBOSE
	bool first = true;
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
#if DEBUG_LEVEL >= VERBOSE
		if (err != 0)
		{
			logmsg (0, LOG_DEBUG,
				"Packet invalid, error is 0x%x", err);
		}
#endif
		uint16_t len =
			tes_ifring_len (rxring, self->heads[ring_id]);
		uint16_t flen = tespkt_flen (pkt);
		if (flen > len)
		{
#if DEBUG_LEVEL >= VERBOSE
			logmsg (0, LOG_DEBUG,
				"Packet too long (header says %hu, "
				"ring slot is %hu)", flen, len);
#endif
			err |= TES_EETHLEN;
			flen = len;
		}
		dbg_assert (flen <= TESPKT_MTU);

		uint16_t cur_fseq = tespkt_fseq (pkt);
		fseq_gap = cur_fseq - self->prev_fseq - 1;
#if DEBUG_LEVEL >= VERBOSE
		if (first)
			dbg_assert (fseq_gap == missed);
		first = false;
		self->dbg_stats.pkts.rcvd_in[ring_id]++;
		self->dbg_stats.pkts.missed += fseq_gap;
#endif

		int rc = self->pkt_handler (loop,
			pkt, flen, fseq_gap, err, self);

		self->prev_fseq = cur_fseq;
		if (tespkt_is_mca (pkt))
			self->prev_pseq_mca = tespkt_pseq (pkt);
		else if (tespkt_is_trace_long (pkt))
			self->prev_pseq_tr = tespkt_pseq (pkt);

		if (rc != 0)
			return rc; /* pkt_handler doesn't want more */
	}

	return 0;
}

static int
s_endp_sub_send (task_endp_t* endpoint, char cmd, char* pattern)
{
	if (endpoint->type == ZMQ_SUB)
	{
		if (cmd == 1)
			zsock_set_subscribe (endpoint->sock, pattern);
		if (cmd == 0)
			zsock_set_unsubscribe (endpoint->sock, pattern);
		else
			assert (false);
	}
	else if (endpoint->type == ZMQ_XSUB)
	{
		size_t len = strlen (pattern);
		char* msg = malloc (len + 2);
		if (msg == NULL)
			return TASK_ERROR;
		int rc = snprintf (msg, len + 2, "%c%s", cmd, pattern);
		if (rc < 0)
			return TASK_ERROR;
		assert ((size_t)rc == len + 1);
		rc = zstr_send (endpoint->sock, msg);
		if (rc == -1)
			return TASK_ERROR;
	}
	else
		assert (false);
	return 0;
}

static void
s_endp_sub_add (task_endp_t* endpoint, char* pattern)
{
	endpoint->pub.nsubs++;
	logmsg (0, LOG_DEBUG, "Subscription for '%s'", pattern);

	void* item = zlistx_add_end (endpoint->pub.subscriptions, pattern);
	if (item == NULL)
		logmsg (0, LOG_WARNING, "Could not insert pattern into list");
}

static void
s_endp_sub_del (task_endp_t* endpoint, char* pattern)
{
	dbg_assert (endpoint->pub.nsubs > 0);
	endpoint->pub.nsubs--;
	logmsg (0, LOG_DEBUG, "Unsubscription for '%s'", pattern);

	void* item = zlistx_find (endpoint->pub.subscriptions, pattern);
	if (item == NULL)
		logmsg (0, LOG_WARNING, "Pattern not in list");
	else if (zlistx_delete (endpoint->pub.subscriptions, item) == -1)
		logmsg (0, LOG_WARNING, "Could not delete pattern from list");
}
