/*
 * TO DO:
 *  - add a flag when configuration has changed (either replacing TICK
 *    or set to another entry in the first vector of the frame)
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
// #define DEFER_EMPTY

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
 *    during the coincidence (before the last non-tick event in the
 *    window)
#endif
 * BAD (coincidence vector):
 *  - measurement is not peak and there are multiple peaks within
 *    one of the events in the coincidence group
#if TICK_WITH_COINC > 0
 * TICK (coincidence vector):
 *  - coincidence is first after a tick
 * TICK (tick vector):
 *  - if n > 1 ticks occured between coincidences, there'd be n-1 all
 *    TOK_TICK vectors with this flag
#endif
 */
#define FLAG_MASK    0xE0
#define UNRESOLVED   (1 << 7)
#define BAD          (1 << 6)
#define TICK_WITH_COINC     0 // 0 or 1
#if TICK_WITH_COINC > 0
#  define TICK       (1 << 5)
#else
#  define TICK              0
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
 * TOK_UNKNOWN, since it would cause an ambiguity. But it can be
 * TOK_NONE, since a vector of all TOK_NONE would never be published
 * as a coincidence.
 */
#define TOK_TICK      0 /* tick vector */
#define TOK_NONE      0 /* no event in this channel */
#define TOK_NOISE   (TES_COINC_MAX_PHOTONS+1) /* measurement below threshold */
#define TOK_UNKNOWN (TES_COINC_MAX_PHOTONS+2) /* an event with no measurement */

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
	uint64_t published; // number of published coincidences
#endif
	struct
	{
		struct
		{
			/* s_add_ticks moves ticks from cur_frame.cur_group to
			 * cur_frame. */
			int num_ongoing; // no. of vectors in the group
			uint16_t ticks;  // since start of group
			uint16_t ticks_since_last;  // since last event
			uint16_t delay_since_start; // since start of group
			uint16_t delay_since_last;	// since relevant event in group
			uint8_t channels;
		} cur_group;
		int idx; // index into coinc of current vector
		int ticks; // no. of TOK_TICK vectors at start of frame
	} cur_frame;
	
	struct s_conf_t conf; // to be read only in s_apply_conf or req_*_hn
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

static int s_check_conf (struct s_conf_t* conf);
static int s_save_conf (task_t* self, struct s_conf_t* conf);
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
static int s_add_to_group (task_t* self);
static int s_add_ticks (task_t* self);
static int s_publish (task_t* self, uint16_t reserve);
static inline bool s_ongoing (struct s_data_t* data);
static inline int  s_num_completed (struct s_data_t* data);

/* -------------------------------------------------------------- */
/* --------------------------- HELPERS -------------------------- */
/* -------------------------------------------------------------- */

/*
 * Check if conf is valid.
 * Returns 0 on success, -1 if conf was invalid.
 */
static int
s_check_conf (struct s_conf_t* conf)
{
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
	return 0;
}

/*
 * Check if conf is valid and if so, copy conf to data.conf, set
 * changed flag and save to file.
 * Returns 0 on success, ECONFINVAL if conf was invalid, ECONFWR if
 * failed to write expected bytes.
 */
#define ECONFINVAL -1
#define ECONFWR     1
static int
s_save_conf (task_t* self, struct s_conf_t* conf)
{
	if (s_check_conf (conf) == -1)
		return ECONFINVAL;
	struct s_data_t* data = (struct s_data_t*) self->data;
	
	memcpy (&data->conf, conf, sizeof (struct s_conf_t));
	data->conf.changed = 1;
	ssize_t rc = task_conf (self, conf, sizeof (struct s_conf_t),
		TES_TASK_SAVE_CONF);
	if (rc == -1 || (size_t)rc != sizeof (struct s_conf_t))
		return ECONFWR;
	return 0;
}

/*
 * Read data.conf and set relevant data. fields used during packet
 * handling.
 * Returns 0 on success, -1 if conf was invalid.
 */
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
#if DEBUG_LEVEL >= LETS_GET_NUTS
		logmsg (0, LOG_DEBUG, "Measurement value is %u", val);
#endif
		uint16_t p = 0;
		for (; val >= (*thres)[p] &&
			p < TES_COINC_MAX_PHOTONS &&
			(p == 0 || (*thres)[p] > 0); p++)
			;
#if DEBUG_LEVEL >= LETS_GET_NUTS
		logmsg (0, LOG_DEBUG, " -> %hhu photons", p);
#endif
		return (p == 0 ? TOK_NOISE : p);
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
	return tespkt_trace_dp (pkt, e);
}

#if 0
static inline uint8_t
s_get_vec_flags (task_t* self, int id)
{
	dbg_assert (self != NULL);
	
	if (id == -1)
		id = self->data->cur_frame.idx;
	dbg_assert (id >= 0);
	
	uint8_t (*cvec)[TES_NCHANNELS] = &self->data->coinc[id];
	return ((*cvec)[0] & FLAG_MASK);
}

static inline void
s_set_vec_flags (task_t* self, uint8_t flags, int id)
{
	dbg_assert (self != NULL);
	
	if (id == -1)
		id = self->data->cur_frame.idx;
	dbg_assert (id >= 0);
	
	uint8_t (*cvec)[TES_NCHANNELS] = &self->data->coinc[id];
	(*cvec)[0] |= flags;
}
#endif

/*
 * Increment idx and num_ongoing. Publish if no more space. Add the
 * UNRESOLVED or TICK flags according to cur_group.num_ongoing and
 * cur_group.ticks, respectively.
 * Returns 0 on success, TASK_ERROR on error.
 */
static int
s_add_to_group (task_t* self)
{
	dbg_assert (self != NULL);
	struct s_data_t* data = (struct s_data_t*) self->data;
	
	uint8_t flags = 0;
	if (data->cur_frame.cur_group.num_ongoing > 0)
	{ /* must be UNRESOLVED */
		dbg_assert (data->coinc[data->cur_frame.idx][0] & UNRESOLVED);
		flags |= UNRESOLVED;
	}
#if TICK_WITH_COINC > 0
	else if (data->cur_frame.cur_group.ticks > 0)
	{ /* add TICK flag if new group follows a tick */
		dbg_assert (data->cur_frame.cur_group.ticks == 1);
		flags |= TICK;
	}
	data->cur_frame.cur_group.ticks = 0;
#else
	else
		dbg_assert (data->cur_frame.cur_group.ticks == 0);
#endif
	dbg_assert (data->cur_frame.cur_group.ticks_since_last == 0);
	
#if DEBUG_LEVEL >= LETS_GET_NUTS
	if (data->cur_frame.cur_group.num_ongoing == 0)
		logmsg (0, LOG_DEBUG, "New group");
	else
		logmsg (0, LOG_DEBUG, "New vector in group");
#endif
	
	if (data->cur_frame.idx == MAX_COINC_VECS - 1)
		if (s_publish (self, 1) == TASK_ERROR)
			return TASK_ERROR;

	data->cur_frame.cur_group.channels = 0;
	data->cur_frame.idx++;
	data->cur_frame.cur_group.num_ongoing++;
	
	uint8_t (*cvec)[TES_NCHANNELS] =
		&data->coinc[data->cur_frame.idx];
	(*cvec)[0] |= flags;
#if DEBUG_LEVEL >= LETS_GET_NUTS
	logmsg (0, LOG_DEBUG, "Added a vector with flags = 0x%02x", flags);
#endif
	return 0;
}

/*
 * Publish all ticks and coincidences so far, then add any ticks since
 * the start of the last coincidence group.
 * Should always be called when a coincidence group has ended after a
 * tick, or when a tick came and no ongoing coincidence was there, and
 * only in those two cases.
 * Returns 0 on success, TASK_ERROR on error.
 */
static int
s_add_ticks (task_t* self)
{
	dbg_assert (self != NULL);
	struct s_data_t* data = (struct s_data_t*) self->data;
	dbg_assert (data->cur_frame.cur_group.num_ongoing == 0);
	assert (data->cur_frame.cur_group.ticks > 0);
	
	int num = data->cur_frame.cur_group.ticks - TICK_WITH_COINC;
	dbg_assert (num >= 0);
	
#ifdef DEFER_EMPTY
	if (s_num_completed (data) > 0 ||
		data->cur_frame.idx + num >= MAX_COINC_VECS)
	{
		if (s_publish (self, num) == TASK_ERROR)
			return TASK_ERROR;
	}
#  if DEBUG_LEVEL >= LETS_GET_NUTS
	else
		logmsg (0, LOG_DEBUG, "Defering publishing");
#  endif
#else
	if (s_publish (self, num) == TASK_ERROR)
		return TASK_ERROR;
#endif
	
	dbg_assert (s_num_completed (data) == 0);
	if (num == 0)
		return 0;
	
	int num_unres = data->cur_frame.cur_group.ticks -
		data->cur_frame.cur_group.ticks_since_last;
	dbg_assert (num_unres >= 0);
	
	dbg_assert (data->cur_frame.idx + num < MAX_COINC_VECS);
	dbg_assert (data->cur_frame.idx + 1 >= 0);
	
	uint8_t (*tick)[TES_NCHANNELS] =
		&data->coinc[data->cur_frame.idx + 1];
	memset (tick, TOK_TICK, num*TES_NCHANNELS);
	for (int t = 0; t < num; t++, tick++)
	{
		(*tick)[0] |= ( t < num_unres ? UNRESOLVED : 0 | TICK );
#if DEBUG_LEVEL >= LETS_GET_NUTS
		logmsg (0, LOG_DEBUG, "Added a   tick with flags = 0x%02x",
			((*tick)[0] & FLAG_MASK));
#endif
	}
	
	data->cur_frame.idx += num;
	data->cur_frame.ticks += num;
	data->cur_frame.cur_group.ticks -= num;
	data->cur_frame.cur_group.ticks_since_last = 0;
	
	if (data->conf.changed)
		s_apply_conf (data);
	
	return 0;
}

/*
 * Publish all completed coincidences, reserving room for <reserve> no.
 * of vectors.
 * If not enough room would be freed by completed, force publish ongoing
 * coincidence, saving only the last vector.
 * Returns 0 on success, TASK_ERROR on error.
 */
static int
s_publish (task_t* self, uint16_t reserve)
{
	dbg_assert (self != NULL);
	struct s_data_t* data = (struct s_data_t*) self->data;
	dbg_assert (reserve + 1 < MAX_COINC_VECS);
	
	int num_ready = s_num_completed (data) + data->cur_frame.ticks;
	dbg_assert (num_ready >= 0);

	if (data->cur_frame.idx + reserve - num_ready >= MAX_COINC_VECS)
	{
		logmsg (0, LOG_DEBUG,
			"Too many vectors in current group");
		/* Keep at least one, since s_add_to_group needs to know whether
		 * to set UNRESOLVED. */
		num_ready = MAX_COINC_VECS - 1;
		data->cur_frame.cur_group.num_ongoing = 1;
	}
	
	if (num_ready == 0)
	{
		assert (data->cur_frame.idx < MAX_COINC_VECS);
		return 0;
	}
	
	/* Publishing only happens if buffer is full, or when a coincidence
	 * completes after a tick. */
	dbg_assert (data->cur_frame.idx + reserve >= MAX_COINC_VECS ||
		data->cur_frame.cur_group.num_ongoing == 0);
#if DEBUG_LEVEL >= LETS_GET_NUTS
	if (data->cur_frame.idx + reserve >= MAX_COINC_VECS)
		logmsg (0, LOG_DEBUG, "Buffer full");
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
#if DEBUG_LEVEL >= VERBOSE
	else
	{
		data->published++;
		if (data->published % 50 == 0)
			logmsg (0, LOG_DEBUG,
				"Published 50 more coincidences");
	}
#endif

	/* TO DO: employ circular buffer instead of moving */
	if (data->cur_frame.cur_group.num_ongoing > 0)
	{ /* move ongoing coincidence vectors to 0 */
		assert (num_ready + data->cur_frame.cur_group.num_ongoing <=
			MAX_COINC_VECS);
		memmove (&data->coinc[0], &data->coinc[num_ready],
			CVEC_SIZE * data->cur_frame.cur_group.num_ongoing);
	}
	data->cur_frame.idx -= num_ready;
	assert (data->cur_frame.idx + 1 >= 0);
	assert (data->cur_frame.idx ==
		data->cur_frame.cur_group.num_ongoing - 1);
	int idx_next = data->cur_frame.idx + 1;
	memset (&data->coinc[idx_next], TOK_NONE,
		CVEC_SIZE * (MAX_COINC_VECS - idx_next));
	
	return 0;
}

static inline bool
s_ongoing (struct s_data_t* data)
{
	return (data->cur_frame.cur_group.num_ongoing > 0);
}

static inline int
s_num_completed (struct s_data_t* data)
{
	return (data->cur_frame.idx + 1 - data->cur_frame.ticks -
		data->cur_frame.cur_group.num_ongoing);
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

	rc = s_save_conf (self, &conf);
	if (rc == ECONFINVAL)
		logmsg (0, LOG_DEBUG,
			"Not changing configuration");
	else
		logmsg (0, LOG_INFO,
			"Setting measurement to %hhu, window to %hu",
			conf.measurement, conf.window);

	if (rc == ECONFWR)
	{
		logmsg (errno, LOG_WARNING,
			"Could not save configuration");
		// return TASK_ERROR;
	}
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
			"Invalid channel number %hhu", channel);
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

		rc = s_save_conf (self, &conf);
		if (rc == ECONFINVAL)
		{
			logmsg (0, LOG_INFO,
				"Invalid configuration");
			req_rc = TES_COINC_REQ_TH_EINV;
		}
		else
			logmsg (0, LOG_INFO,
				"Setting new thresholds for measurement %hhu on channel %hhu",
				meas, channel);

		if (rc == ECONFWR)
		{
			logmsg (errno, LOG_WARNING,
				"Could not save configuration");
			// return TASK_ERROR;
		}
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
 * Wait for the first tick, and then for the start of a new
 * coincidence (event delay > window) before adding any coincidence
 * vectors.
 *
 * If the event type contains the relevant measurement, save the
 * counts. If the channel has been seen before, start a new vector in
 * the same coincidence group, set the UNRESOLVED flag.
 *
 * If the event type does not contain the relevant measurement set the
 * channel count to TOK_UNKNOWN.
 *
 * Either way, if it's a tick, and if DEFER_EMPTY is not set or there
 * have been completed coincidences since the last publishing, publish
 * the tick, followed by any completed coincidences.
 * If DEFER_EMPTY is not set, there may be only tick vectors. Either
 * way, there may be only the coincidence frame if we are publishing
 * because the buffer is full.
 */
int
task_coinc_pkt_hn (zloop_t* loop, tespkt* pkt, uint16_t flen,
		uint16_t missed, int err, task_t* self)
{
	dbg_assert (self != NULL);
	struct s_data_t* data = (struct s_data_t*) self->data;
	dbg_assert (data->cur_frame.idx < MAX_COINC_VECS);
	dbg_assert (data->cur_frame.cur_group.ticks >=
		data->cur_frame.cur_group.ticks_since_last);
	dbg_assert (s_num_completed (data) >= 0);
	dbg_assert (data->publishing || ! s_ongoing (data));

	if ( err || ! tespkt_is_event (pkt) )
		return 0;

#if DEBUG_LEVEL >= LETS_GET_NUTS
	logmsg (0, LOG_DEBUG, "------------------------------");
#endif

	bool is_tick = tespkt_is_tick (pkt);
	if (is_tick)
	{
		data->cur_frame.cur_group.ticks++;
		data->cur_frame.cur_group.ticks_since_last++;
		if ( ! s_ongoing (data) )
			return s_add_ticks (self);
	}
	else if ( ! data->publishing && data->cur_frame.ticks == 0)
		return 0; /* no ticks yet */

	dbg_assert (! is_tick /* set counts */
		|| ( ! data->publishing &&
			data->cur_frame.ticks > 0 ) /* wait for coinc start */
		|| s_ongoing (data) /* add to delay_since_* */
		);

	for (int e = 0; e < tespkt_event_nums (pkt); e++)
	{
		uint16_t delay = tespkt_event_toff (pkt, e);
		data->cur_frame.cur_group.delay_since_last += delay;
		data->cur_frame.cur_group.delay_since_start += delay;
		struct tespkt_event_flags* ef = tespkt_evt_fl (pkt, e);
		
#if DEBUG_LEVEL >= LETS_GET_NUTS
		logmsg (0, LOG_DEBUG, "Channel %hhu %s, delay is %hu",
			ef->CH, is_tick ? "tick " : "frame", delay);
		logmsg (0, LOG_DEBUG, "Delay since start: %hu, since last: %hu",
			data->cur_frame.cur_group.delay_since_start,
			data->cur_frame.cur_group.delay_since_last);
#endif
		
		if (data->cur_frame.cur_group.delay_since_last > data->window)
		{
			/* Start publishing if not doing so already, since this begins a
			 * new coincidence after a tick. */
			data->publishing = true;
			
			if (s_ongoing (data))
			{ /* ongoing group ends */
#if DEBUG_LEVEL >= LETS_GET_NUTS
				logmsg (0, LOG_DEBUG, "Group ends");
#endif
				data->cur_frame.cur_group.num_ongoing = 0;
				if (data->cur_frame.cur_group.ticks > 0 &&
					s_add_ticks (self) == TASK_ERROR)
					return TASK_ERROR;
			}
		}

		if (is_tick)
			break;
			
		data->cur_frame.cur_group.delay_since_last = 0;
		data->cur_frame.cur_group.ticks_since_last = 0;
		
		if ( ! data->publishing )
			continue; /* wait for start of next coincidence */
			
		/* Start a new coincidence vector if this is the first measurement
		 * event since last group concluded or if the current channel has
		 * been seen in this coincidence group. */
		bool add_vec = 0;
		if (! s_ongoing (data))
		{ /* new group */
			data->cur_frame.cur_group.delay_since_start = 0;
			add_vec = true;
		}
		else
		{ /* ongoing group continues */
			bool ch_seen =
				data->cur_frame.cur_group.channels & (1 << ef->CH);
			
			if (ch_seen ||
				(data->cur_frame.cur_group.delay_since_start > data->window))
			{
				/* The second case, delay_since_start > window, would apply
				 * only if there have been more than one vectors in this group
				 * so far, or if there are more than two channels. */
				dbg_assert (ch_seen ||
					(data->cur_frame.cur_group.num_ongoing > 1 ||
						TES_NCHANNELS > 2));
#if DEBUG_LEVEL >= LETS_GET_NUTS
				if (data->cur_frame.cur_group.num_ongoing == 1)
					logmsg (0, LOG_DEBUG, "Group becomes unresolved");
#endif
				uint8_t (*cvec)[TES_NCHANNELS] =
					&data->coinc[data->cur_frame.idx];
				(*cvec)[0] |= UNRESOLVED;
			}
			
			if (ch_seen)
			{ /* new vector in the group */
				add_vec = true;
			}
		}

		if (add_vec && s_add_to_group (self) == TASK_ERROR)
			return TASK_ERROR;

		dbg_assert (data->cur_frame.idx < MAX_COINC_VECS);
		dbg_assert (data->cur_frame.idx >= 0);
		dbg_assert (s_ongoing (data));
		
		data->cur_frame.cur_group.channels |= (1 << ef->CH);
		uint8_t (*cvec)[TES_NCHANNELS] =
			&data->coinc[data->cur_frame.idx];
		uint32_t (*thres)[TES_COINC_MAX_PHOTONS] =
			&(*data->thresholds)[ef->CH];
		
		if (data->util.is_bad (pkt, e))
		{
#if DEBUG_LEVEL >= LETS_GET_NUTS
			logmsg (0, LOG_DEBUG, "Measurement is bad");
#endif
			(*cvec)[0] |= BAD;
		}
		dbg_assert (((*cvec)[ef->CH] & (~FLAG_MASK)) == 0);
		(*cvec)[ef->CH] |= data->util.get_counts (pkt, e, thres);
	}

	return 0;
}

int
task_coinc_init (task_t* self)
{
	assert (self != NULL);
	assert (TES_NCHANNELS <= 8); // cur_group.channels is uint8
	assert (TES_NCHANNELS == 2 || TES_NCHANNELS == 4 ||
		TES_NCHANNELS == 8); // has to be a power of 2
	assert (TICK_WITH_COINC == 0 || TICK_WITH_COINC == 1);
	assert (self->frontends[ENDP_REP].type == ZMQ_REP);
	assert (self->frontends[ENDP_REP_TH].type == ZMQ_REP);
	assert (self->frontends[ENDP_PUB].type == ZMQ_XPUB);
	assert ((UNRESOLVED & FLAG_MASK) == UNRESOLVED);
	assert ((BAD & FLAG_MASK) == BAD);
	assert ((TICK & FLAG_MASK) == TICK);
	assert ((TOK_TICK & FLAG_MASK) == 0);
	assert ((TOK_NONE & FLAG_MASK) == 0);
	assert ((TOK_NOISE & FLAG_MASK) == 0);
	assert ((TOK_UNKNOWN & FLAG_MASK) == 0);
	assert ((TES_COINC_MAX_PHOTONS & FLAG_MASK) == 0);

	static struct s_data_t data;
	assert (sizeof (data.coinc[0]) == CVEC_SIZE);
	assert (sizeof (data.coinc) == MAX_COINC_VECS*CVEC_SIZE);
	assert (TES_COINC_MAX_SIZE == MAX_COINC_VECS*CVEC_SIZE);
	
	self->data = &data;

	struct s_conf_t conf = {0};
	ssize_t rc = task_conf (self, &conf, sizeof (struct s_conf_t),
		TES_TASK_READ_CONF);
	if (rc == 0)
		return 0; /* config disabled */
	
	bool set_default = false;
	if ((size_t)rc != sizeof (struct s_conf_t))
	{
		if (rc != -1)
			logmsg (0, LOG_WARNING,
				"Read unexpected number of bytes from config file: %lu", rc);
		set_default = true;
	}
	else
	{
		if (s_check_conf (&conf) == -1)
		{
			logmsg (0, LOG_WARNING,
					"Read invalid configuration");
			set_default = true;
		}
		else
			memcpy (&data.conf, &conf, sizeof (struct s_conf_t));
	}

	if (set_default)
	{
		/* Thresholds of all zero means 1 threshold = 0. */
		conf.window = 100;
		conf.measurement = TES_COINC_MEAS_AREA;
		rc = s_save_conf (self, &conf);
		assert (rc != ECONFINVAL);
		if (rc == ECONFWR)
		{
			logmsg (errno, LOG_WARNING,
				"Could not save configuration");
			// return TASK_ERROR;
		}
	}

	s_apply_conf (&data);

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
	data->cur_frame.idx = -1;
	return 0;
}
