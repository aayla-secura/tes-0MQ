#include "tesd_tasks.h"

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

static zloop_timer_fn  s_task_avgtr_timeout_hn;

/* ------------------------------------------------------------------------- */

int
task_avgtr_req_hn (zloop_t* loop, zsock_t* reader, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;

	uint32_t timeout;	

	int rc = zsock_recv (reader, TAVGTR_REQ_PIC, &timeout);
	if (rc == -1)
	{ /* would also return -1 if picture contained a pointer (p) or a null
	   * frame (z) but message received did not match this signature; this
	   * is irrelevant in this case */
		logmsg (0, LOG_DEBUG, "Receive interrupted");
		return TASK_ERROR;
	}

	/* Check timeout. */
	if (timeout == 0)
	{
		logmsg (0, LOG_INFO,
			"Received a malformed request");
		zsock_send (self->frontend, TAVGTR_REP_PIC_FAIL,
				TAVGTR_REQ_INV);
		return 0;
	}

	logmsg (0, LOG_INFO,
		"Received request for a trace in the next %u seconds",
		timeout);

	/* Register a timer */
	int tid = zloop_timer (loop, 1000 * timeout, 1,
		s_task_avgtr_timeout_hn, self);
	if (tid == -1)
	{
		logmsg (errno, LOG_ERR,
			"Could not set a timer");
		return TASK_ERROR;
	}
	struct s_task_avgtr_data_t* trace =
		(struct s_task_avgtr_data_t*) self->data;
	dbg_assert ( ! trace->recording );
	trace->timer = tid;

	/* Disable polling on the reader until the job is done. Wakeup packet
	 * handler. */
	task_activate (self);

	return 0;
}

/*
 * Accumulates average trace frames. As soon as a complete trace is
 * recorded, it is sent to the client, polling on the client reader is
 * re-enabled and the timer is canceled. It aborts the whole trace if
 * a relevant frame is lost, and waits for the next one.
 */
int
task_avgtr_pkt_hn (zloop_t* loop, tespkt* pkt, uint16_t flen,
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
		logmsg (0, LOG_DEBUG,
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
			logmsg (0, LOG_DEBUG,
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
			logmsg (0, LOG_INFO,
				"Discarded average trace");
			zsock_send (self->frontend, TAVGTR_REP_PIC_FAIL,
					TAVGTR_REQ_ERR);
			break;
		case TAVGTR_REQ_OK:
			logmsg (0, LOG_INFO,
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

int
task_avgtr_init (task_t* self)
{
	assert (self != NULL);

	static struct s_task_avgtr_data_t trace;

	self->data = &trace;
	return 0;
}

int
task_avgtr_fin (task_t* self)
{
	assert (self != NULL);

	self->data = NULL;
	return 0;
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
	task_deactivate (self);
	
	/* Send a timeout error to the client. */
	logmsg (0, LOG_INFO,
		"Average trace timed out");
	zsock_send (self->frontend, TAVGTR_REP_PIC_FAIL,
			TAVGTR_REQ_TOUT);

	return 0;
}
