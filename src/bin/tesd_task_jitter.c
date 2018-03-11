/*
 * TO DO: cache histograms so subsequent subscribers can get the last
 * completed one.
 */

#include "tesd_tasks.h"

/* Need to keep track of all non-ref frames between two neighbouring
 * reference frames. */
#define MAX_SIMULT_POINTS 16
#define ENDP_REP 0
#define ENDP_PUB 1
#ifdef ENABLE_FULL_DEBUG
#  define MAGIC_REQ_TICKS 0
#  define MAGIC_REQ_REFCH UINT8_MAX
#endif

/* Add to delay, so bin 0 is underflow and middle bin is 0 delay. */
#define BIN_OFFSET 512

#define CONF_LEN 16
struct s_conf_t
{
	uint64_t ticks;  // publish and reset after that many
	uint8_t  ref_ch; // reference channel
};

/*
 * Data for currently built histogram.
 */
struct s_data_t
{
	struct s_conf_t cur_conf; // current configuration
	struct s_conf_t conf;     // to be applied at next hist
#ifdef ENABLE_FULL_DEBUG
	uint64_t published;    // number of published histograms
	uint64_t dropped;      // number of aborted histograms
	bool     testing;      // magic request will enable this
#endif
	uint32_t nsubs;        // no. of subscribers at any time
	uint32_t bins[TES_JITTER_NBINS];
	uint64_t ticks;        // number of ticks so far
	struct {
		uint16_t delay_since; // delay since last reference
		uint16_t delay_until; // delay until next reference
	} points[MAX_SIMULT_POINTS];
	uint8_t  cur_npts;     // no. of non-ref frames since last ref + 1
	bool     publishing;   // discard all frames until first tick
};

static void s_reset (struct s_data_t* hist);

/* -------------------------------------------------------------- */
/* --------------------------- HELPERS -------------------------- */
/* -------------------------------------------------------------- */

static void
s_reset (struct s_data_t* hist)
{
	dbg_assert (hist != NULL);

	memcpy (&hist->cur_conf, &hist->conf, CONF_LEN);
	memset (&hist->bins, 0, sizeof (hist->bins));
	/* No need to zero hist->points, each new point when first added is
	 * set to the greatest dealy. */
	hist->ticks = 0;
	hist->publishing = 0;
	/* Last entry in hist->points is now first in the queue. */
	hist->points[0] = hist->points[hist->cur_npts - 1];
	hist->cur_npts = 1;
}

/* -------------------------------------------------------------- */
/* ----------------------------- API ---------------------------- */
/* -------------------------------------------------------------- */

int
task_jitter_req_hn (zloop_t* loop, zsock_t* frontend, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;

	uint8_t ref_ch;
	uint64_t ticks;
	int rc = zsock_recv (frontend, TES_JITTER_REQ_PIC,
		&ref_ch, &ticks);
	/* Would also return -1 if picture contained a pointer (p) or a null
	 * frame (z) but message received did not match this signature; this
	 * is irrelevant in this case; we don't get interrupted, this should
	 * not happen. */
	assert (rc != -1);

	struct s_data_t* hist = (struct s_data_t*) self->data;
#ifdef ENABLE_FULL_DEBUG
	if (ticks == MAGIC_REQ_TICKS && ref_ch == MAGIC_REQ_REFCH)
	{
		ticks = 1;
		ref_ch = 0;
		hist->testing = 1;
	}
	else
#endif
	/* FIX: how many channels are there? */
	if (ticks == 0 || ref_ch > 1)
	{
		logmsg (0, LOG_INFO,
			"Received a malformed request");
		zsock_send (frontend, TES_JITTER_REP_PIC, TES_JITTER_REQ_EINV);
		return 0;
	}

	logmsg (0, LOG_INFO,
		"Using channel %u as reference, publishing each %lu ticks",
		ref_ch, ticks);

	hist->conf.ref_ch = ref_ch;
	hist->conf.ticks = ticks;

	return 0;
}

/*
 * XPUB will receive a message of the form "\x01<prefix>" the first time
 * a client subscribes to the port with a prefix <prefix>, and will
 * receive a message of the form "\x00<prefix>" when the last client
 * subscribed to <prefix> unsubscribes.
 * It will also receive any message sent to the port (by an ill-behaved
 * client) that does not begin with "\x00" or "\x01", these should be
 * ignored.
 */
int
task_jitter_sub_hn (zloop_t* loop, zsock_t* frontend, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;

	zmsg_t* msg = zmsg_recv (frontend);
	if (msg == NULL)
	{ /* this shouldn't happen */
		logmsg (0, LOG_DEBUG, "Receive interrupted");
		return TASK_ERROR;
	}

	if (zmsg_size (msg) != 1)
	{
		logmsg (0, LOG_DEBUG,
			"Got a spurious %lu-frame message", zmsg_size(msg));
		zmsg_destroy (&msg);
		return 0;
	}

#ifdef ENABLE_FULL_DEBUG
	zframe_t* f = zmsg_first (msg);
	char* hexstr = zframe_strhex (f);
	logmsg (0, LOG_DEBUG,
		"Got message %s", hexstr);
	zstr_free (&hexstr);
#endif

	struct s_data_t* hist = (struct s_data_t*) self->data;
	char* msgstr = zmsg_popstr (msg);
	zmsg_destroy (&msg);
	char stat = msgstr[0];
	zstr_free (&msgstr);
	if (stat == 0)
	{
		dbg_assert (hist->nsubs > 0);
		hist->nsubs--;
	}
	else if (stat == 1)
	{
		hist->nsubs++;
	}
	else
	{
		logmsg (0, LOG_DEBUG,
			"Got a spurious message");
		return 0;
	}

	if (hist->nsubs == 1)
	{
		logmsg (0, LOG_DEBUG,
			"First subscription, activating");
		/* Wakeup packet handler. */
		s_reset (hist);
	 /* Wait for first reference frame. */
		/* hist->points[0] = {.delay_since = 0, .delay_until = 0}; */
		hist->points[0].delay_since = 0;
		hist->points[0].delay_until = 0;
		hist->cur_npts = 0;
		task_activate (self);
	}
	else if (hist->nsubs == 0)
	{
		logmsg (0, LOG_DEBUG,
			"Last unsubscription, deactivating");
		/* Deactivate packet handler. */
		task_deactivate (self);
	}

	return 0;
}

/*
 * When a reference non-tick frame comes, it adds to delay_until and
 * saved all points being tracked.
 *
 * When a non-reference non-tick frame comes a new point in the tracked
 * list is added with the delay_since of the previous one, and the delay
 * of the current frame is added to delay_since for all tracked points.
 *
 * When a tick comes it adds to the delay_since for all tracked points.
 */
int
task_jitter_pkt_hn (zloop_t* loop, tespkt* pkt, uint16_t flen,
		uint16_t missed, int err, task_t* self)
{
	dbg_assert (self != NULL);

	struct s_data_t* hist = (struct s_data_t*) self->data;
	dbg_assert (hist->cur_conf.ticks > 0);

	bool is_tick = tespkt_is_tick (pkt);
	if ( ! hist->publishing && is_tick )
		hist->publishing = 1; /* start accumulating */

	if ( ! hist->publishing || err || ! tespkt_is_event (pkt) )
		return 0;

	if (is_tick)
		hist->ticks++;

	uint16_t delay = tespkt_event_toff (pkt);
	struct tespkt_event_flags* ef = tespkt_evt_fl (pkt);

	if (hist->testing)
	{
		logmsg (0, LOG_DEBUG, "Channel %hhu frame%s, delay is %hu",
			ef->CH, is_tick ? " (tick)" : "       ", delay);
		for (uint8_t p = 0; p < hist->cur_npts; p++)
			logmsg (0, LOG_DEBUG, "Point %hhu delays: %hu, %hu",
					p, hist->points[p].delay_since,
					hist->points[p].delay_until);
	}

	if (ef->CH == hist->cur_conf.ref_ch && ! is_tick)
	{ /* reference frame, save points */
		/* Last entry in hist->points is to be discarded. */
		dbg_assert (hist->cur_npts < MAX_SIMULT_POINTS);
		for (uint8_t p = 0; p < hist->cur_npts - 1; p++)
		{
			if ((uint64_t)hist->points[p].delay_until + delay > UINT16_MAX)
				hist->points[p].delay_until = UINT16_MAX;
			else
				hist->points[p].delay_until += delay;

			int64_t bin = hist->points[p].delay_since;
			if (bin > (int64_t)hist->points[p].delay_until)
				bin = - hist->points[p].delay_until;

			if (hist->testing)
				logmsg (0, LOG_DEBUG, "Added a point at %hd", bin);

			bin += BIN_OFFSET;
			if (bin < 0)
				bin = 0;
			else if (bin >= TES_JITTER_NBINS)
				bin = TES_JITTER_NBINS - 1;
			
			hist->bins[bin]++;
#ifdef ENABLE_FULL_DEBUG
			if (hist->bins[bin] == 0)
				logmsg (0, LOG_WARNING, "Overflow of bin %hd", bin);
#endif
			hist->points[p].delay_since = 0;
			hist->points[p].delay_until = 0;
		}
		/* Start accumulating delay for next non-reference frame. */
		dbg_assert (hist->points[0].delay_until == 0);
		hist->cur_npts = 1;
	}
	else
	{ /* non-reference frame */
		if (hist->cur_npts == 0)
			return 0; /* waiting for first ref since wake-up */

		if ( ! is_tick )
		{ /* a new point to keep track of */
			if (hist->cur_npts < MAX_SIMULT_POINTS - 1)
			{ /* last non-reference has the greatest delay, use it as a start */
				hist->points[hist->cur_npts].delay_since =
					hist->points[hist->cur_npts - 1].delay_since;
				hist->points[hist->cur_npts].delay_until = 0;
				if (hist->testing)
					logmsg (0, LOG_DEBUG, "Added a new point");
				hist->cur_npts++;
			}
#ifdef ENABLE_FULL_DEBUG
			else
			{
				logmsg (0, LOG_DEBUG,
					"Too many non-reference frames since last reference");
			}
#endif
		}

		dbg_assert (hist->cur_npts < MAX_SIMULT_POINTS);
		for (uint8_t p = 0; p < hist->cur_npts; p++)
		{
			if ((uint64_t)hist->points[p].delay_since + delay > UINT16_MAX)
				hist->points[p].delay_since = UINT16_MAX;
			else
				hist->points[p].delay_since += delay;
		}
	}

	if (hist->ticks == hist->cur_conf.ticks + 1)
	{ /* publish histogram */
		int rc = zmq_send (
			zsock_resolve (self->frontends[ENDP_PUB].sock),
			(void*)hist->bins, TES_JITTER_NBINS * TES_JITTER_BIN_LEN, 0);
#ifdef ENABLE_FULL_DEBUG
		if (rc == -1)
		{
			logmsg (errno, LOG_ERR,
				"Cannot send the histogram");
			return TASK_ERROR;
		}

		hist->published++;
		if (hist->published % 50)
			logmsg (0, LOG_DEBUG,
				"Published 50 more histogtams");
#endif

		s_reset (hist);
	}

	dbg_assert (hist->ticks <= hist->cur_conf.ticks);
	return 0;
}

int
task_jitter_init (task_t* self)
{
	assert (self != NULL);
	assert (sizeof (struct s_conf_t) == CONF_LEN);
	assert (BIN_OFFSET == (TES_JITTER_NBINS - 1) / 2);

	static struct s_data_t hist;
	assert (sizeof (hist.bins[0]) == TES_JITTER_BIN_LEN);

	/* Some defaults. */
	hist.conf.ticks = 5;
	hist.conf.ref_ch = 0;

#ifdef ENABLE_FULL_DEBUG
	hist.testing = 1;
#endif

	self->data = &hist;
	return 0;
}

int
task_jitter_fin (task_t* self)
{
	assert (self != NULL);

	self->data = NULL;
	return 0;
}
