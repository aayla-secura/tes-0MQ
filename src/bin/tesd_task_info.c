/*
 * TODO:
 *  - Make endpoint a ROUTER and send average statistics. If a new
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
	uint64_t mcas;   // header frames only
	uint64_t traces; // header frames only
	uint64_t events; // non-tick, non-trace
	uint8_t  event_types; // each set bit is a type that was seen
};

/*
 * Event_types is 0 if no events were seen.
 * See api.h for resulting bit-offset.
 */
#define linear_etype(pkt_type,tr_type) \
	( (pkt_type == TESPKT_TYPE_TRACE) ? 4 + tr_type : pkt_type + 1 )

static zloop_timer_fn  s_timeout_hn;

/* -------------------------------------------------------------- */
/* --------------------------- HELPERS -------------------------- */
/* -------------------------------------------------------------- */

/*
 * Deactivates the task, enables polling on the client endpoint, sends
 * stats to client.
 */
static int
s_timeout_hn (zloop_t* loop, int timer_id, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;
	assert (self->data != NULL);
	struct s_data_t* info = (struct s_data_t*) self->data;

	/* Enable polling on the endpoint and deactivate packet
	 * handler. */
	task_deactivate (self);
	
	/* Send a timeout error to the client. */
	logmsg (0, LOG_INFO,
		"Packets: %lu processed, "
		"%lu missed, "
		"%lu bad, "
		"%lu ticks, "
		"%lu mcas, "
		"%lu traces, "
		"%lu other events",
		info->received,
		info->missed,
		info->bad,
		info->ticks,
		info->mcas,
		info->traces,
		info->events);
	zsock_send (self->endpoints[0].sock, TES_INFO_REP_PIC,
		TES_INFO_REQ_OK,
		info->received,
		info->missed,
		info->bad,
		info->ticks,
		info->mcas,
		info->traces,
		info->events,
		info->event_types);

	memset (info, 0, sizeof (struct s_data_t));

	return 0;
}

/* -------------------------------------------------------------- */
/* ----------------------------- API ---------------------------- */
/* -------------------------------------------------------------- */

int
task_info_req_hn (zloop_t* loop, zsock_t* endpoint, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;
	assert (self->data != NULL);

	uint32_t timeout;

	int rc = zsock_recv (endpoint, TES_INFO_REQ_PIC, &timeout);
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
		zsock_send (endpoint, TES_INFO_REP_PIC,
			TES_INFO_REQ_EINV, 0, 0, 0, 0, 0, 0, 0, 0);
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

	/* Disable polling on the endpoint until the job is done. Wakeup
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
	dbg_assert (self->data != NULL);
	struct s_data_t* info = (struct s_data_t*) self->data;

	bool is_tick = tespkt_is_tick (pkt);
	bool is_header = tespkt_is_header (pkt);
	bool is_tr_header = (tespkt_is_trace_long (pkt) && is_header) ||
		tespkt_is_trace_dp (pkt); /* FIXME: should trace_dp count here */
	bool is_mca_header = tespkt_is_mca (pkt) && is_header;
	bool is_event = (tespkt_is_event (pkt) && ! is_tick);

	info->received++;
	info->missed += missed;
	if (err)
		info->bad++;
	else if (is_tick)
		info->ticks++;
	else if (is_mca_header)
		info->mcas++;
	else if (is_tr_header)
		info->traces++;
	else if (is_event) // and not a trace
		info->events += tespkt_event_nums (pkt);

	if (is_event) // including trace
	{
		const struct tespkt_event_type* etype = tespkt_etype (pkt);
		info->event_types |= (1 << linear_etype (etype->PKT, etype->TR));
	}

	return 0;
}

int
task_info_init (task_t* self)
{
	assert (self != NULL);

	assert (linear_etype (TESPKT_TYPE_PEAK, 0) == TES_INFO_ETYPE_PEAK);
	assert (linear_etype (TESPKT_TYPE_AREA, 0) == TES_INFO_ETYPE_AREA);
	assert (linear_etype (TESPKT_TYPE_PULSE, 0) == TES_INFO_ETYPE_PULSE);
	assert (linear_etype (TESPKT_TYPE_TRACE, TESPKT_TRACE_TYPE_SGL) ==
		TES_INFO_ETYPE_TRACE_SGL);
	assert (linear_etype (TESPKT_TYPE_TRACE, TESPKT_TRACE_TYPE_AVG) ==
		TES_INFO_ETYPE_TRACE_AVG);
	assert (linear_etype (TESPKT_TYPE_TRACE, TESPKT_TRACE_TYPE_DP) ==
		TES_INFO_ETYPE_TRACE_DP);
	assert (linear_etype (TESPKT_TYPE_TRACE, TESPKT_TRACE_TYPE_DPTR) ==
		TES_INFO_ETYPE_TRACE_DPTR);

	static struct s_data_t info;
	self->data = &info;

	return 0;
}
