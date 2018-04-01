/*
 * TO DO:
 *  - Make frontend a ROUTER and send average statistics. If a new
 *    request comes with a timeout less than already elapsed, send reply
 *    immediately.
 *    Set a separate timer for each client.
 */

#include "tesd_tasks.h"

/*
 * Statistics accumulated over requested period.
 */
struct s_data_t
{
	uint64_t received;
	uint64_t missed;
	uint64_t bad;
	uint64_t ticks;
	uint64_t mcas;
	uint64_t traces;
	uint64_t events;
};

static zloop_timer_fn  s_timeout_hn;

/* -------------------------------------------------------------- */
/* --------------------------- HELPERS -------------------------- */
/* -------------------------------------------------------------- */

/*
 * Deactivates the task, enables polling on the client frontend, sends
 * stats to client.
 */
static int
s_timeout_hn (zloop_t* loop, int timer_id, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;
	struct s_data_t* info = (struct s_data_t*) self->data;

	/* Enable polling on the frontend and deactivate packet
	 * handler. */
	task_deactivate (self);
	
	/* Send a timeout error to the client. */
	logmsg (0, LOG_INFO,
		"Packets: %lu processed, "
		"%lu missed, "
		"%lu bad, "
		"%lu ticks, "
		"%lu mcas, "
		"%lu traces"
		"%lu other events",
		info->received,
		info->missed,
		info->bad,
		info->ticks,
		info->mcas,
		info->traces);
	zsock_send (self->frontends[0].sock, TES_INFO_REP_PIC,
		TES_INFO_REQ_OK,
		info->received,
		info->missed,
		info->bad,
		info->ticks,
		info->mcas,
		info->traces,
		info->events);

	memset (info, 0, sizeof (struct s_data_t));

	return 0;
}

/* -------------------------------------------------------------- */
/* ----------------------------- API ---------------------------- */
/* -------------------------------------------------------------- */

int
task_info_req_hn (zloop_t* loop, zsock_t* frontend, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;

	uint32_t timeout;

	int rc = zsock_recv (frontend, TES_INFO_REQ_PIC, &timeout);
	/* Would also return -1 if picture contained a pointer (p) or a null
	 * frame (z) but message received did not match this signature; this
	 * is irrelevant in this case; we don't get interrupted, this should
	 * not happen. */
	assert (rc != -1);

	/* Check timeout. */
	if (timeout == 0)
	{
		logmsg (0, LOG_INFO,
			"Received a malformed request");
		zsock_send (frontend, TES_INFO_REP_PIC,
			TES_INFO_REQ_EINV, 0, 0, 0, 0, 0);
		return 0;
	}

	logmsg (0, LOG_INFO,
		"Received request for packet info over the next %u seconds",
		timeout);

	/* Register a timer */
	int tid = zloop_timer (loop, 1000 * timeout, 1, s_timeout_hn, self);
	if (tid == -1)
	{
		logmsg (errno, LOG_ERR,
			"Could not set a timer");
		return TASK_ERROR;
	}

	/* Disable polling on the frontend until the job is done. Wakeup
	 * packet handler. */
	task_activate (self);

	return 0;
}

/*
 * Accumulates packet info.
 * Always returns 0.
 */
int
task_info_pkt_hn (zloop_t* loop, tespkt* pkt, uint16_t flen,
		uint16_t missed, int err, task_t* self)
{
	dbg_assert (self != NULL);
	struct s_data_t* info = (struct s_data_t*) self->data;

	bool is_header = tespkt_is_header (pkt);
	bool is_tr_header = (tespkt_is_trace_long (pkt) && is_header) ||
		tespkt_is_trace_dp (pkt); /* FIX: should trace_dp count here */
	bool is_mca_header = tespkt_is_mca (pkt) && is_header;

	info->received++;
	info->missed += missed;
	if (err)
		info->bad++;
	else if (tespkt_is_tick (pkt))
		info->ticks++;
	else if (is_mca_header)
		info->mcas++;
	else if (is_tr_header)
		info->traces++;
	else if (tespkt_is_event (pkt))
		info->events += tespkt_event_nums (pkt);

	return 0;
}

int
task_info_init (task_t* self)
{
	assert (self != NULL);

	static struct s_data_t info;

	self->data = &info;
	return 0;
}

int
task_info_fin (task_t* self)
{
	assert (self != NULL);

	self->data = NULL;
	return 0;
}
