/*
 * TO DO:
 *  - FIX: discard the first coincidence if it starts before the first tick
 */

#include "tesd_tasks.h"

#define MAX_COINC_VECS 256
#define CVEC_SIZE   TES_NCHANNELS // elements are one byte
#define ENDP_REP    0
#define ENDP_REP_TH 1
#define ENDP_PUB    2

/*
 * If DEFER_EMPTY is set, do not publish when a tick comes unless
 * there have been completed coincidences since the last one.
 */
/* #define DEFER_EMPTY */

/*
 * Flags in the first byte of a vector.
 * A tick vector is a vector with all elements (masked for flags) ==
 * TOK_TICK; all other vectors are coincidence vectors, see definitions
 * of TOK_*.
 *
 * UNRESOLVED (coincidence vector):
 *  - consecutive (relevant for the measurement) events each within
 *    less than window, w, delay from the previous, but last one is
 *    delayed more than w since first
 *  - two events in the same channel within a coincidence group
 * UNRESOLVED (tick vector):
 *  - tick occured during the previous coincidence (there may be
 *    multiple consecutive tick vectors with this flag, but never an
 *    unresolved tick following a resolved one with no coincidences in
 *    between
#if TICK_WITH_COINC > 0
 *    the UNRESOLVED flag will not be applied to the coincidence vector
 *    with the tick flag set, even if that tick occured during the
 *    coincidence; it will only be set for the extra, all TOK_TICK,
 *    vectors before that coincidence, if any of them also occured
 *    during the coincidence
#endif
 * BAD (coincidence vector):
 *  - measurement is not peak and there are multiple peaks within
 *    one of the events in the coincidence group
#if TICK_WITH_COINC == 0
 * TICK (coincidence vector):
 *  - coincidence is first after a tick
 * TICK (tick vector):
 *  - if n > 1 ticks occured between coincidences, there'd be n-1 all
 *    TOK_TICK vectors with this flag
#endif
 */
#define UNRESOLVED   (1 << 7)
#define BAD          (1 << 6)
#define TICK_WITH_COINC     0 // or 1
#if TICK_WITH_COINC > 0
#  define TICK              0
#else
#  define TICK       (1 << 5)
#endif

/*
 * Maximum number of thresholds is 16, meaning that maximum photon
 * number is 17 (meaning 17 or more photons). No event in the channel is
 * distinguished from event with photon number 0 (below lowest
 * threshold). Also distinguished is a channel in which an event came
 * but didn't contain a measurement (e.g. an average trace).
 *
 * A vector of all TOK_TICK is a tick vector, otherwise it's
 * a coincidence vector.
 * TOK_TICK cannot be between 1 and 17, and it cannot be TOK_NOISE or
 * TOK_UNKNOWN, since it would cause an ambiguity. But it can be TOK_NONE,
 * since a vector of all TOK_NONE would never be published as
 * a coincidence.
 */
#define TOK_TICK      0 /* tick vector */
#define TOK_NONE      0 /* no event in this channel */
#define TOK_NOISE   '-' /* measurement below threshold */
#define TOK_UNKNOWN '?' /* an event with no measurement */

typedef bool (s_frame_check_fn)(tespkt* pkt);
typedef bool (s_event_check_fn)(tespkt* pkt, uint16_t event);
typedef int  (s_count_fn)(tespkt* pkt, uint16_t event,
	uint32_t (*threshold)[TES_COINC_MAX_PHOTONS]);

#define NUM_MEAS 3
struct s_conf_t
{
	uint32_t thresholds[NUM_MEAS][TES_NCHANNELS][TES_COINC_MAX_PHOTONS];
	uint16_t window;
	uint8_t  measurement;
	bool changed; // since last application
}; /* saved and re-read at startup */

/*
 * Data for currently built coincidence stream.
 */
struct s_data_t
{
#if DEBUG_LEVEL >= VERBOSE
	uint64_t published;    // number of published histograms
	uint64_t dropped;      // number of aborted histograms
#endif
	struct
	{
		struct
		{
			int num_ongoing; // no. of vectors in the group
			int ticks; // since start of group
			uint16_t delay_since_start; // since start of group
			uint16_t delay_since_last;	// since relevant event in group
			uint8_t channels;
		} cur_group;
		int idx; // index into coinc of current vector
		int ticks; // no. of ticks at start of frame
	} cur_frame;
	
	struct s_conf_t conf;
	/* following is assigned when changing conf */
	uint32_t (*thresholds)[TES_NCHANNELS][TES_COINC_MAX_PHOTONS];
	uint16_t window;
	struct
	{
		s_event_check_fn* is_bad;
		s_count_fn*	get_counts;
	} util;
	
	uint8_t coinc[MAX_COINC_VECS][TES_NCHANNELS]; // includes ticks
	bool publishing; // discard all coincidences before first tick
};

static int  s_save_conf (struct s_data_t* data,
	struct s_conf_t* conf);
static void s_apply_conf (struct s_data_t* data);
static inline uint16_t s_count_from_thres (uint32_t val,
	uint32_t (*thres)[TES_COINC_MAX_PHOTONS]);
static inline s_frame_check_fn s_has_area;
static inline s_frame_check_fn s_has_peak;
static inline s_frame_check_fn s_has_dp;
static s_event_check_fn s_is_bad_area;
static s_event_check_fn s_is_bad_peak;
static s_event_check_fn s_is_bad_dp;
static s_count_fn s_from_area;
static s_count_fn s_from_peak;
static s_count_fn s_from_dp;
static int s_add_to_group (task_t* self, uint8_t flags);
static int s_add_ticks (task_t* self, int n, uint8_t flags);
static int s_publish (task_t* self);

/* -------------------------------------------------------------- */
/* --------------------------- HELPERS -------------------------- */
/* -------------------------------------------------------------- */

static int
s_save_conf (struct s_data_t* data, struct s_conf_t* conf)
{
	dbg_assert (data != NULL);
	dbg_assert (conf != NULL);
	
	if (conf->measurement >= NUM_MEAS ||
		conf->window == 0 ||
		conf->window > TES_COINC_MAX_WINDOW)
		return -1;
	
	for (int m = 0; m < NUM_MEAS; m++)
		for (int c = 0; c < TES_NCHANNELS; c++)
			for (int p = 1, rest_is_zero = 0;
				p < TES_COINC_MAX_PHOTONS; p++)
				if (conf->thresholds[m][c][p] > 0 &&
					(rest_is_zero || conf->thresholds[m][c][p] <=
						conf->thresholds[m][c][p-1]))
						return -1;
				else if (conf->thresholds[m][c][p] == 0)
					rest_is_zero = 1;
	
	memcpy (&data->conf, conf, sizeof (struct s_conf_t));
	data->conf.changed = 1;
	/* FIX: save conf to file */
	return 0;
}

static void
s_apply_conf (struct s_data_t* data)
{
	dbg_assert (data != NULL);
	data->window = data->conf.window;
	data->thresholds = &data->conf.thresholds[data->conf.measurement];
	switch (data->conf.measurement)
	{
		case TES_COINC_MEAS_AREA:
			data->util.is_bad = s_is_bad_area;
			data->util.get_counts = s_from_area;
			break;
		case TES_COINC_MEAS_PEAK:
			data->util.is_bad = s_is_bad_peak;
			data->util.get_counts = s_from_peak;
			break;
		case TES_COINC_MEAS_DOTP:
			data->util.is_bad = s_is_bad_dp;
			data->util.get_counts = s_from_dp;
			break;
		default:
			assert (false);
	}
	data->conf.changed = 0;
}

static inline uint16_t
s_count_from_thres (uint32_t val,
	uint32_t (*thres)[TES_COINC_MAX_PHOTONS])
{
		uint16_t p = 0;
		for (; val >= (*thres)[p] &&
			p < TES_COINC_MAX_PHOTONS &&
			(p == 0 || (*thres)[p] > 0); p++)
			;
		return p;
}

static inline bool
s_has_area (tespkt* pkt)
{
	return (tespkt_is_area (pkt) || tespkt_is_pulse (pkt) ||
		(tespkt_is_trace (pkt) && ! tespkt_is_trace_avg (pkt)));
}

static inline bool
s_has_peak (tespkt* pkt)
{
	return (tespkt_is_peak (pkt) || tespkt_is_multipeak (pkt));
}

static inline bool
s_has_dp (tespkt* pkt)
{
	return (tespkt_is_trace_dp (pkt) || tespkt_is_trace_dptr (pkt));
}

static bool
s_is_bad_area (tespkt* pkt, uint16_t e)
{
	return (tespkt_peak_nums (pkt, e) > 1);
}

static bool
s_is_bad_peak (tespkt* pkt, uint16_t e)
{
	return 0;
}

static bool
s_is_bad_dp (tespkt* pkt, uint16_t e)
{
	return (tespkt_peak_nums (pkt, e) > 1);
}

static int
s_from_area (tespkt* pkt, uint16_t e,
	uint32_t (*thres)[TES_COINC_MAX_PHOTONS])
{
	if ( ! s_has_area (pkt) )
		return TOK_UNKNOWN;
	return s_count_from_thres (tespkt_event_area (pkt, e), thres);
}

static int
s_from_peak (tespkt* pkt, uint16_t e,
	uint32_t (*thres)[TES_COINC_MAX_PHOTONS])
{
	if ( ! s_has_peak (pkt) )
		return TOK_UNKNOWN;
	if (tespkt_is_multipeak (pkt))
		return s_count_from_thres (
			tespkt_multipeak_height (pkt, e, 0), thres);
	else
		return s_count_from_thres (tespkt_peak_height (pkt, e), thres);
}

static int
s_from_dp (tespkt* pkt, uint16_t e,
	uint32_t (*thres)[TES_COINC_MAX_PHOTONS])
{
	if ( ! s_has_dp (pkt) )
		return TOK_UNKNOWN;
	/* FIX: TO DO */
	return TOK_UNKNOWN;
}

static int
s_add_to_group (task_t* self, uint8_t flags)
{
	dbg_assert (self != NULL);
	struct s_data_t* data = (struct s_data_t*) self->data;
	
	/* If the UNRESOLVED bit is to be set for the first time in this
	 * group, it is because num_ongoing == 1, and vice versa. */
	uint8_t (*cvec)[TES_NCHANNELS] = &data->coinc[data->cur_frame.idx];
	if (data->cur_frame.cur_group.num_ongoing == 1)
		dbg_assert ((flags & UNRESOLVED) &&
			! ((*(cvec-1))[0] & UNRESOLVED));
	else if (data->cur_frame.cur_group.num_ongoing > 1)
		dbg_assert ((flags & UNRESOLVED) &&
			((*(cvec-1))[0] & UNRESOLVED));
	(*cvec)[0] |= flags;

#if DEBUG_LEVEL >= ARE_YOU_NUTS
	if (data->cur_frame.cur_group.num_ongoing == 0)
		logmsg (0, LOG_DEBUG, "New group");
	else
		logmsg (0, LOG_DEBUG, "New vector in group");
#endif

#if TICK > 0
	if (data->cur_frame.cur_group.ticks > 0 &&
		data->cur_frame.cur_group.num_ongoing == 0)
	{
		dbg_assert (data->cur_frame.cur_group.ticks == 1);
		flags |= TICK;
	}
	data->cur_frame.cur_group.ticks = 0;
#else
	dbg_assert (data->cur_frame.cur_group.ticks == 0 ||
		data->cur_frame.cur_group.num_ongoing > 0);
#endif
	
	data->cur_frame.cur_group.channels = 0;
	data->cur_frame.idx++;
	data->cur_frame.cur_group.num_ongoing++;
	
	if (data->cur_frame.idx == MAX_COINC_VECS)
		return s_publish (self);
	return 0;
}

static int
s_add_ticks (task_t* self, int n, uint8_t flags)
{
	dbg_assert (self != NULL);
	dbg_assert (n > 0);
	
	struct s_data_t* data = (struct s_data_t*) self->data;
	dbg_assert (data->cur_frame.cur_group.num_ongoing == 0);
	
#ifdef DEFER_EMPTY
	if (data->cur_frame.idx + 1 > data->cur_frame.ticks)
	{
		if (s_publish (self) == TASK_ERROR)
			return TASK_ERROR;
	}
#else
	if (s_publish (self) == TASK_ERROR)
		return TASK_ERROR;
#endif
	
	dbg_assert (data->cur_frame.idx ==
		data->cur_frame.cur_group.num_ongoing - 1);
	dbg_assert (data->cur_frame.idx + n < MAX_COINC_VECS);
	dbg_assert (data->cur_frame.idx + 1 >= 0);
	
	if (data->conf.changed)
		s_apply_conf (data);
	
	uint8_t (*tick)[TES_NCHANNELS] =
		&data->coinc[data->cur_frame.idx + 1];
	memset (tick, TOK_TICK, n*TES_NCHANNELS);
	for (int t = 0; t < n; t++)
		(*tick)[t] |= flags | TICK;
	
	data->cur_frame.idx += n;
	if (flags & UNRESOLVED)
	{
		data->cur_frame.cur_group.ticks -= n;
		dbg_assert (data->cur_frame.cur_group.ticks == TICK_WITH_COINC);
	}
	else
		dbg_assert (data->cur_frame.cur_group.ticks == 0);
	data->cur_frame.ticks += n;
	
	return 0;
}

static int
s_publish (task_t* self)
{
	dbg_assert (self != NULL);
	struct s_data_t* data = (struct s_data_t*) self->data;
	
	int num_ready =
		data->cur_frame.idx - data->cur_frame.cur_group.num_ongoing + 1;
	dbg_assert (num_ready >= 0);

	if (data->cur_frame.idx - num_ready >= MAX_COINC_VECS)
	{
		logmsg (0, LOG_DEBUG,
			"Too many vectors in current group");
		/* Keep at least two, since s_add_to_group treats first two
		 * differently. */
		num_ready = MAX_COINC_VECS - 2;
		data->cur_frame.cur_group.num_ongoing = 2;
	}
	if (num_ready == 0)
	{
		assert (data->cur_frame.idx < MAX_COINC_VECS);
		return 0;
	}
	
#if DEBUG_LEVEL >= ARE_YOU_NUTS
	logmsg (0, LOG_DEBUG,
		"Publishing frame with %d ticks", data->cur_frame.ticks);
#endif
	data->cur_frame.ticks = 0;

	int rc = zmq_send (
		zsock_resolve (self->frontends[ENDP_PUB].sock),
		(void*)&data->coinc[0], CVEC_SIZE * num_ready, 0);
	if (rc == -1)
	{
		logmsg (errno, LOG_ERR,
			"Cannot send the coincidence");
		return TASK_ERROR;
	}

	/* TO DO: employ circular buffer instead of moving */
	if (data->cur_frame.cur_group.num_ongoing > 0)
	{ /* move ongoing coincidence vectors to 0 */
		memmove (&data->coinc[0], &data->coinc[num_ready],
			CVEC_SIZE * data->cur_frame.cur_group.num_ongoing);
	}
	data->cur_frame.idx -= num_ready;
	dbg_assert (data->cur_frame.idx >= 0 ||
		(data->cur_frame.idx == -1 &&
			data->cur_frame.cur_group.num_ongoing == 0));
	int idx_next = data->cur_frame.idx + 1;
	memset (&data->coinc[idx_next], TOK_NONE,
		CVEC_SIZE * (MAX_COINC_VECS - idx_next));
	
	return 0;
}

/* -------------------------------------------------------------- */
/* ----------------------------- API ---------------------------- */
/* -------------------------------------------------------------- */

int
task_coinc_req_hn (zloop_t* loop, zsock_t* frontend, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;
	struct s_data_t* data = (struct s_data_t*) self->data;
	struct s_conf_t conf = {0};
	memcpy (&conf, &data->conf, sizeof (struct s_conf_t));
	
	int rc = zsock_recv (frontend, TES_COINC_REQ_PIC,
		&conf.window, &conf.measurement);
	/* Would also return -1 if picture contained a pointer (p) or a null
	 * frame (z) but message received did not match this signature; this
	 * is irrelevant in this case; we don't get interrupted, this should
	 * not happen. */
	assert (rc != -1);

	if (s_save_conf (data, &conf) == -1)
		logmsg (0, LOG_DEBUG,
			"Not changing configuration");
	zsock_send (frontend, TES_COINC_REP_PIC,
		data->conf.window, data->conf.measurement);
	
	return 0;
}

int
task_coinc_req_th_hn (zloop_t* loop, zsock_t* frontend, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;
	struct s_data_t* data = (struct s_data_t*) self->data;
	
	uint8_t meas, channel;
	char* buf;
	size_t len;
	int rc = zsock_recv (frontend, TES_COINC_REQ_TH_PIC,
		&meas, &channel, &buf, &len);
	/* Would also return -1 if picture contained a pointer (p) or a null
	 * frame (z) but message received did not match this signature; this
	 * is irrelevant in this case; we don't get interrupted, this should
	 * not happen. */
	assert (rc != -1);
	
	bool invalid = false;
	if (buf == NULL)
		assert (len == 0); /* query current conf */
	if (len % 4 != 0)
	{
		logmsg (0, LOG_INFO,
			"Received malformed threshold data, size is %lu", len);
		invalid = true;
	}
	else if (meas >= NUM_MEAS)
	{
		logmsg (0, LOG_INFO,
			"Invalid measurement id %hhu", meas);
		invalid = true;
	}
	else if (channel >= TES_NCHANNELS)
	{
		logmsg (0, LOG_INFO,
			"Invalid channel number %hhu", meas);
		invalid = true;
	}
	
	if (invalid)
	{
		zsock_send (frontend, TES_COINC_REP_TH_PIC,
			TES_COINC_REQ_TH_EINV, "", 0);
		zstr_free (&buf);
		return 0;
	}
	
	dbg_assert (len % 4 == 0 && buf != NULL && meas < NUM_MEAS
		&& channel < TES_NCHANNELS);
	uint8_t req_rc = TES_COINC_REQ_TH_OK;
	if (len > 0)
	{ /* update config */
		struct s_conf_t conf = {0};
		memcpy (&conf, &data->conf, sizeof (struct s_conf_t));
		uint32_t (*thres)[TES_COINC_MAX_PHOTONS] =
			&conf.thresholds[meas][channel];
		memset (thres, 0, sizeof (*thres));
		memcpy (thres, buf, len);
		if (s_save_conf (data, &conf) == -1)
		{
			logmsg (0, LOG_INFO,
				"Invalid configuration");
			req_rc = TES_COINC_REQ_TH_EINV;
		}
		else
			logmsg (0, LOG_INFO,
				"Setting new thresholds");
	}
	else
	{
		logmsg (0, LOG_DEBUG,
			"Not changing configuration");
	}
	zstr_free (&buf);
	
	uint32_t (*thres)[TES_COINC_MAX_PHOTONS] =
		&data->conf.thresholds[meas][channel];
	zsock_send (frontend, TES_COINC_REP_TH_PIC,
		req_rc, thres, sizeof (*thres));
	
	return 0;
}

/*
 * If the event type contains the relevant measurement, save the
 * counts. If the channel has been seen before, start a new vector in
 * the same coincidence group, set the UNRESOLVED flag.
 *
 * If the event type does not contain the relevant measurement and
 * there is an ongoing coincidence, set the channel count to
 * TOK_UNKNOWN.
 *
 * Either way, if it's a tick, and if DEFER_EMPTY is not set or there
 * have been completed coincidences since the last publishing, publish
 * the tick, followed by any completed coincidences. It is sent as a
 * multi-frame message with ticks in the first frame(s), and all
 * coincidences in the last frame. If DEFER_EMPTY is not set, there
 * may be only tick frames. Either way, there may be only the
 * coincidence frame if we are publishing because the buffer is full.
 */
int
task_coinc_pkt_hn (zloop_t* loop, tespkt* pkt, uint16_t flen,
		uint16_t missed, int err, task_t* self)
{
	dbg_assert (self != NULL);
	struct s_data_t* data = (struct s_data_t*) self->data;
	dbg_assert (data->cur_frame.idx < MAX_COINC_VECS);
	dbg_assert (data->cur_frame.idx + 1 >= data->cur_frame.ticks +
		data->cur_frame.cur_group.num_ongoing);

	bool is_tick = tespkt_is_tick (pkt);
	if ( ! data->publishing && is_tick )
	{
		data->publishing = true; /* start accumulating */
		dbg_assert (data->cur_frame.cur_group.num_ongoing == 0);
	}
	
	/* FIX: don't return if not publishing */
	if ( ! data->publishing || err || ! tespkt_is_event (pkt) )
		return 0;

	bool ongoing_coinc = data->cur_frame.cur_group.num_ongoing != 0;
	dbg_assert ( (
			data->cur_frame.cur_group.delay_since_last == 0 &&
			data->cur_frame.cur_group.channels == 0)
		|| ongoing_coinc);

	if (is_tick)
	{
		if (ongoing_coinc)
			data->cur_frame.cur_group.ticks++;
		else if (s_add_ticks (self, 1, 0) == TASK_ERROR)
			return TASK_ERROR;
	}

	if (is_tick && ! ongoing_coinc)
			return 0; /* no ongoing coincidence */

	dbg_assert (! is_tick /* set counts */
		|| ongoing_coinc    /* add to delay_since_* */
		);

	for (int e = 0; e < tespkt_event_nums (pkt); e++)
	{
		uint16_t delay = tespkt_event_toff (pkt, e);
		data->cur_frame.cur_group.delay_since_last += delay;
		data->cur_frame.cur_group.delay_since_start += delay;
		struct tespkt_event_flags* ef = tespkt_evt_fl (pkt, e);
		
		/* Start a new coincidence vector if the current channel has been
		 * seen in this coincidence group, or if this is the first
		 * measurement event since last group concluded. */
		if (data->cur_frame.cur_group.delay_since_last >
				data->window)
		{ /* ongoing group ends */
			data->cur_frame.cur_group.num_ongoing = 0;
			data->cur_frame.cur_group.delay_since_start = 0;
			data->cur_frame.cur_group.delay_since_last = 0;
			data->cur_frame.cur_group.channels = 0;
			if (data->cur_frame.cur_group.ticks > TICK_WITH_COINC)
			{
				if (s_add_ticks (self,
						data->cur_frame.cur_group.ticks - TICK_WITH_COINC,
						UNRESOLVED) == TASK_ERROR)
					return TASK_ERROR;
			}
		}
		else if ( ! is_tick )
		{ /* ongoing group continues */
			bool ch_seen =
				data->cur_frame.cur_group.channels & (1 << ef->CH);
			uint8_t flags = 0;
			if (ch_seen || (data->cur_frame.cur_group.delay_since_start >
					data->window))
				flags = UNRESOLVED;
			
			if (ch_seen)
			{ /* new vector in the group */
				if (s_add_to_group (self, flags) == TASK_ERROR)
					return TASK_ERROR;
			}
		}

		if (is_tick)
			break;
			
#if DEBUG_LEVEL >= ARE_YOU_NUTS
		logmsg (0, LOG_DEBUG, "Channel %hhu frame, delay is %hu",
			ef->CH, delay);
#endif
		
		if (data->cur_frame.cur_group.num_ongoing == 0)
		{ /* new group */
			if (s_add_to_group (self, 0) == TASK_ERROR)
				return TASK_ERROR;
		}

		dbg_assert (data->cur_frame.idx < MAX_COINC_VECS);
		dbg_assert (data->cur_frame.idx >= 0);
		dbg_assert (data->cur_frame.cur_group.num_ongoing > 0);
		data->cur_frame.cur_group.delay_since_last = 0;
		
		data->cur_frame.cur_group.channels |= (1 << ef->CH);
		uint8_t (*cvec)[TES_NCHANNELS] =
			&data->coinc[data->cur_frame.idx];
		uint32_t (*thres)[TES_COINC_MAX_PHOTONS] =
			&(*data->thresholds)[ef->CH];
		
		if (data->util.is_bad (pkt, e))
			(*cvec)[0] |= BAD;
		(*cvec)[ef->CH] = data->util.get_counts (pkt, e, thres);
#if DEBUG_LEVEL >= ARE_YOU_NUTS
		logmsg (0, LOG_DEBUG, "  %hhu photons", (*cvec)[ef->CH]);
#endif
	}

	return 0;
}

int
task_coinc_init (task_t* self)
{
	assert (self != NULL);
	assert (TES_NCHANNELS < 8); // data.cur_frame.cur_group.channels is uint8
	assert (TICK_WITH_COINC == 0 || TICK_WITH_COINC == 1);
	assert (self->frontends[ENDP_REP].type == ZMQ_REP);
	assert (self->frontends[ENDP_REP_TH].type == ZMQ_REP);
	assert (self->frontends[ENDP_PUB].type == ZMQ_XPUB);

	static struct s_data_t data;
	assert (sizeof (data.coinc[0]) == CVEC_SIZE);
	assert (sizeof (data.coinc) == MAX_COINC_VECS*CVEC_SIZE);
	assert (TES_COINC_MAX_SIZE == MAX_COINC_VECS*CVEC_SIZE);
	
	/* Default conf. Thresholds of all zero means 1 threshold = 0. */
	data.conf.window = 100;
	data.conf.measurement = TES_COINC_MEAS_AREA;
	s_apply_conf (&data);
	/* FIX read conf */

	self->data = &data;
	return 0;
}

int
task_coinc_wakeup (task_t* self)
{
	assert (self != NULL);
	struct s_data_t* data = (struct s_data_t*) self->data;

	memset (&data->coinc, TOK_NONE, MAX_COINC_VECS*CVEC_SIZE);
	memset (&data->cur_frame, 0, sizeof (data->cur_frame));
	data->publishing = false;
	return 0;
}

int
task_coinc_fin (task_t* self)
{
	assert (self != NULL);

	self->data = NULL;
	return 0;
}
