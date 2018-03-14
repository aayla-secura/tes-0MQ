/*
 * TO DO:
 *  - cache histograms so subsequent subscribers can get the last
 *    completed one.
 *  - determine number of hists at runtime (add dynamically)?
 */

#include "tesd_tasks.h"

/* Need to keep track of all non-ref frames between two neighbouring
 * reference frames. */
#define MAX_SIMULT_POINTS 16
#define ENDP_REP 0
#define ENDP_PUB 1

/* Add to delay, so bin 0 is underflow and middle bin is 0 delay. */
#define BIN_OFFSET 511

#define CONF_LEN 16
struct s_conf_t
{
	uint64_t ticks;  // publish and reset after that many
	uint8_t  ref_ch; // reference channel
};

struct s_point_t
{
	/* Even though each frame's delay saturates at UINT16_MAX, we allow
	 * for larger accumulated delay, so we select the correct overflow bin
	 * (positive vs negative) more often. */
	uint16_t delay_since; // delay since last reference
	uint16_t delay_until; // delay until next reference
	int      hid;         // histogram (0 to TES_JITTER_NHISTS-1)
};

struct s_hist_hdr_t
{
	uint64_t : 48; /* reserved */
	uint8_t ref_ch;
	uint8_t nhists;
} __attribute__ ((__packed__));

struct s_subhist_t
{
	struct
	{
		uint64_t : 56; /* reserved */
		uint8_t ch;
	} hdr;
	uint32_t bins[TES_JITTER_NBINS];
};

struct s_hist_t
{
	struct s_hist_hdr_t hdr;
	struct s_subhist_t hists[TES_JITTER_NHISTS];
};

/*
 * Data for currently built histogram.
 */
struct s_data_t
{
	struct s_conf_t cur_conf; // current configuration
	struct s_conf_t conf;     // to be applied at next hist
#if DEBUG_LEVEL >= VERBOSE
	uint64_t published;    // number of published histograms
	uint64_t dropped;      // number of aborted histograms
#endif
	uint32_t nsubs;        // no. of subscribers at any time
	struct s_hist_t hist;
	uint64_t ticks;        // number of ticks so far
	struct s_point_t points[MAX_SIMULT_POINTS];
	uint8_t  cur_npts;     // no. of non-ref frames since last ref + 1
	bool     publishing;   // discard all frames until first tick
};

static inline void s_add_to_since (struct s_point_t* pt, uint16_t delay);
static inline void s_add_to_until (struct s_point_t* pt, uint16_t delay);
static inline void s_save_points (struct s_data_t* data);
static void s_prep_next (struct s_data_t* data);
static void s_reset (struct s_data_t* data);

/* -------------------------------------------------------------- */
/* --------------------------- HELPERS -------------------------- */
/* -------------------------------------------------------------- */

static inline void
s_add_to_since (struct s_point_t* pt, uint16_t delay)
{
		if (pt->delay_since + delay > UINT16_MAX)
			pt->delay_since = UINT16_MAX;
		else
			pt->delay_since += delay;
}

static inline void
s_add_to_until (struct s_point_t* pt, uint16_t delay)
{
		if (pt->delay_until + delay > UINT16_MAX)
			pt->delay_until = UINT16_MAX;
		else
			pt->delay_until += delay;
}

/*
 * Called on a reference frame.
 */
static inline void
s_save_points (struct s_data_t* data)
{
	dbg_assert (data != NULL);

	for (uint8_t p = 0; p < data->cur_npts - 1; p++)
	{
		struct s_point_t* pt = &data->points[p];
		int bin = pt->delay_since;
		if (bin > pt->delay_until)
			bin = - pt->delay_until;

#if DEBUG_LEVEL >= ARE_YOU_NUTS
		logmsg (0, LOG_DEBUG, "Added a point at %ld", bin);
#endif

		bin += BIN_OFFSET;
		if (bin < 0)
			bin = 0;
		else if (bin >= TES_JITTER_NBINS)
			bin = TES_JITTER_NBINS - 1;
		
		dbg_assert (pt->hid < TES_JITTER_NHISTS);
		dbg_assert (pt->hid >= 0);
		struct s_subhist_t* hist = &data->hist.hists[pt->hid];
		hist->bins[bin]++;
#if DEBUG_LEVEL >= VERBOSE
		if (data->hist.hists[pt->hid].bins[bin] == 0)
			logmsg (0, LOG_WARNING, "Overflow of bin %hd", bin);
#endif
	}
	/* Start accumulating delay for next non-reference frame. */
	data->points[0].delay_since = 0;
	data->points[0].delay_until = 0;
	data->points[0].hid = -1;
	data->cur_npts = 1;
}

/*
 * Called on publishing or activation.
 */
static void
s_prep_next (struct s_data_t* data)
{
	dbg_assert (data != NULL);

	memcpy (&data->cur_conf, &data->conf, CONF_LEN);
	memset (&data->hist, 0, sizeof (data->hist));
	data->hist.hdr.ref_ch = data->conf.ref_ch;
	data->hist.hdr.nhists = TES_JITTER_NHISTS;
	for (int h = 0, ch = 0; h < TES_JITTER_NHISTS; h++, ch++)
	{
		if (ch == data->conf.ref_ch)
			ch++;
		data->hist.hists[h].hdr.ch = ch;
	}
	/* No need to zero data->points, each new point when first added is
	 * set to the greatest dealy. */
	data->ticks = 0;
	if (data->publishing)
	{
		dbg_assert (data->cur_npts > 0);
		/* Last entry in hist->points is now first in the queue. */
		data->points[0] = data->points[data->cur_npts - 1];
		dbg_assert(data->points[0].hid == -1);
		data->cur_npts = 1;
		data->ticks = 1;
	}
	else
		dbg_assert (data->cur_npts == 0);
}

/*
 * Called on activation.
 */
static void
s_reset (struct s_data_t* data)
{
	dbg_assert (data != NULL);

	memset (&data->points, 0, sizeof (data->points));
	/* Wait for first tick and reference frame. */
	data->publishing = 0;
	data->cur_npts = 0;
	s_prep_next (data);
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

	struct s_data_t* data = (struct s_data_t*) self->data;
	if (ticks == 0 || ref_ch > TES_JITTER_NHISTS)
	{
		logmsg (0, LOG_DEBUG,
			"Not changing configuration");
	}
	else
	{
		logmsg (0, LOG_INFO,
			"Using channel %u as reference, publishing each %lu ticks",
			ref_ch, ticks);

		data->conf.ref_ch = ref_ch;
		data->conf.ticks = ticks;
	}

	zsock_send (frontend, TES_JITTER_REP_PIC,
		data->conf.ref_ch, data->conf.ticks);

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
	/* We don't get interrupted. */
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
	logmsg (0, LOG_DEBUG,
		"Got message %s", hexstr);
	zstr_free (&hexstr);
#endif

	struct s_data_t* data = (struct s_data_t*) self->data;
	char* msgstr = zmsg_popstr (msg);
	zmsg_destroy (&msg);
	char stat = msgstr[0];
	zstr_free (&msgstr);
	if (stat == 0)
	{
		dbg_assert (data->nsubs > 0);
		data->nsubs--;
	}
	else if (stat == 1)
	{
		data->nsubs++;
	}
	else
	{
		logmsg (0, LOG_DEBUG,
			"Got a spurious message");
		return 0;
	}

	if (data->nsubs == 1)
	{
		logmsg (0, LOG_DEBUG,
			"First subscription, activating");
		s_reset (data);
		/* Wakeup packet handler. */
		task_activate (self);
	}
	else if (data->nsubs == 0)
	{
		logmsg (0, LOG_DEBUG,
			"Last unsubscription, deactivating");
		/* Deactivate packet handler. */
		task_deactivate (self);
	}

	return 0;
}

/*
 * Reference frames are non-tick frames from the reference channel.
 *
 * At any time the number of currently tracked points is number of
 * non-reference frames since last reference frame + 1 (the last point
 * accumulating delays for the would-be-next non-reference point).
 *
 * Any frame adds its delay to the delay_until of all but the last
 * currently tracked points.
 *
 * In addition to that:
 *  - If the frame is a reference frame, it saves all but the last
 *    tracked points to the histogram and resets them.
 *  - If the frame is a non-reference frame its delay is added to the
 *    delay_since of the last point (would-be-next non-reference point).
 *    In addition to that:
 *     - If the non-reference frame is not a tick, a new point in the
 *       tracked list is added with the delay_since of the last one, and
 *       the delay_until of 0. It is the new would-be-next point.
 */
int
task_jitter_pkt_hn (zloop_t* loop, tespkt* pkt, uint16_t flen,
		uint16_t missed, int err, task_t* self)
{
	dbg_assert (self != NULL);

	struct s_data_t* data = (struct s_data_t*) self->data;
	dbg_assert (data->cur_conf.ticks > 0);
	dbg_assert (data->cur_npts < MAX_SIMULT_POINTS);

	bool is_tick = tespkt_is_tick (pkt);
	if ( ! data->publishing && is_tick )
		data->publishing = 1; /* start accumulating */

	if ( ! data->publishing || err || ! tespkt_is_event (pkt) )
		return 0;

	bool is_trace = tespkt_is_trace_long (pkt);
	if ( is_trace && ! tespkt_is_header (pkt) )
		return 0; /* non-header frame from multi-stream */

	if (is_tick)
		data->ticks++;

	for (int e = 0; e < tespkt_event_nums (pkt); e++)
	{
		if (is_tick || is_trace)
			dbg_assert (e == 0);
		uint16_t delay = tespkt_event_toff (pkt, e);
		struct tespkt_event_flags* ef = tespkt_evt_fl (pkt, e);
		bool is_ref = (ef->CH == data->cur_conf.ref_ch && ! is_tick);
		bool make_new = ( ! is_ref && ! is_tick );

		if ( ! is_ref && data->cur_npts == 0)
			return 0; /* waiting for first ref since wake-up */

		for (uint8_t p = 0; p < data->cur_npts - 1; p++)
			s_add_to_until (&data->points[p], delay);

		/* Do this before printing debug info. */
		if ( ! is_ref )
			s_add_to_since (&data->points[data->cur_npts - 1], delay);

#if DEBUG_LEVEL >= ARE_YOU_NUTS
		logmsg (0, LOG_DEBUG, "Channel %hhu frame%s, delay is %hu",
			ef->CH, is_tick ? " (tick)" : "			 ", delay);
		for (uint8_t p = 0; p < data->cur_npts; p++)
			logmsg (0, LOG_DEBUG, "Point %hhu delays: %hu, %hu",
				p, data->points[p].delay_since,
				data->points[p].delay_until);
#endif

		if (is_ref)
			s_save_points (data);
		// else
		//	 s_add_to_since (&data->points[data->cur_npts - 1], delay);

		dbg_assert (data->cur_npts > 0);
		if (make_new)
		{
			dbg_assert (ef->CH != data->cur_conf.ref_ch);
			if (data->cur_npts < MAX_SIMULT_POINTS - 1)
			{
				struct s_point_t* new_entry = &data->points[data->cur_npts-1];
				int hid = ef->CH;
				if (ef->CH > data->cur_conf.ref_ch)
					hid--;
				new_entry->hid = hid;

				struct s_point_t* new_ghost = new_entry + 1;
				/* Last non-reference has the greatest delay_since,
				 * use it as a start. */
				new_ghost->delay_since = new_entry->delay_since;
				new_ghost->delay_until = 0;
				new_ghost->hid = -1;
				data->cur_npts++;
			}
#if DEBUG_LEVEL >= VERBOSE
			else
			{
				logmsg (0, LOG_WARNING,
					"Too many non-reference frames since last reference");
			}
#endif
		}
	}

	if (data->ticks == data->cur_conf.ticks + 1)
	{ /* publish histogram */
		int rc = zmq_send (
			zsock_resolve (self->frontends[ENDP_PUB].sock),
			(void*)&data->hist, TES_JITTER_SIZE, 0);
#if DEBUG_LEVEL >= VERBOSE
		if (rc == -1)
		{
			logmsg (errno, LOG_ERR,
				"Cannot send the histogram");
			return TASK_ERROR;
		}

		data->published++;
		if (data->published % 50)
			logmsg (0, LOG_DEBUG,
				"Published 50 more histogtams");
#endif

		s_prep_next (data);
	}

	dbg_assert (data->ticks <= data->cur_conf.ticks);
	return 0;
}

int
task_jitter_init (task_t* self)
{
	assert (self != NULL);
	assert (sizeof (struct s_hist_hdr_t) == TES_JITTER_HDR_LEN);
	assert (sizeof (struct s_hist_t) == TES_JITTER_SIZE);
	assert (sizeof (struct s_subhist_t) == TES_JITTER_SUBHDR_LEN +
		TES_JITTER_BIN_LEN*TES_JITTER_NBINS);
	assert (sizeof (struct s_conf_t) == CONF_LEN);
	assert (BIN_OFFSET == (int)((TES_JITTER_NBINS) / 2));

	static struct s_data_t data;
	assert (TES_JITTER_BIN_LEN == sizeof (data.hist.hists[0].bins[0]));

	/* Some defaults. */
	data.conf.ticks = 5;
	data.conf.ref_ch = 0;

	self->data = &data;
	return 0;
}

int
task_jitter_fin (task_t* self)
{
	assert (self != NULL);

	self->data = NULL;
	return 0;
}
