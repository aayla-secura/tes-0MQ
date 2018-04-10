/*
 * TODO:
 *  - set window from config
 *  - check pattern against no. set thresholds
 */

#include "tesd_tasks.h"
#define CVEC_SIZE   TES_NCHANNELS // elements are one byte
#define ENDP_REP    0
#define ENDP_PUB    1
#define ENDP_SUB    2
#define MAX_SUBSC_LEN (3*TES_NCHANNELS)

/*
 * Pattern vectors are sent as characters. Allowed tokens are:
 *  '0':      matches TES_COINC_TOK_NONE or TES_COINC_TOK_NOISE
 *  '1'-'16': matches corresponding 1-16
 *  'N':      matches any of 1-16
 *  '-':      matches TES_COINC_TOK_NOISE
 *  'X':      matches any (including TES_COINC_TOK_UNKNOWN
 * These are transformed into the corresponding values.
 * Analysing the pattern string and transforming it into a vector of
 * numeric tokens is done in task_coinccount_sub_dup.
 * TOK_ANY and TOK_NUM can be anything, as long as after flag mask
 * their value does not equal any valid coincidence token.
 */
#define TOK_NUM 0x1E
#define TOK_ANY 0x1F

#define FIRST_SUB(self) (struct s_subscription_t*) \
		zhashx_first (self->endpoints[ENDP_PUB].pub.subscriptions)
#define NEXT_SUB(self)  (struct s_subscription_t*) \
		zhashx_next (self->endpoints[ENDP_PUB].pub.subscriptions)

typedef uint8_t coinc_vec_t[TES_NCHANNELS];

struct s_subscription_t
{
	coinc_vec_t pattern;
	char pattern_str[MAX_SUBSC_LEN];
	struct
	{
		uint64_t num_res_match;
		uint64_t num_res_match_noMP;
		uint64_t num_res;
		uint64_t num_res_noMP;
		uint64_t num_unres;
		uint32_t ticks; // re-read at 'activation' and when publishing
		uint32_t cur_ticks;
	} counts;
	uint32_t ticks;  // set at subscription; if 0, read global
	bool active;     // just subscribed, waiting for its tick
	bool is_private; // start at next tick; otherwise wait for next
	                 // batch of global counts
};

struct s_data_t
{
	uint32_t ticks;      // globally synchronized patterns
	uint32_t cur_ticks;  // globally synchronized patterns
	uint32_t next_ticks; // config applied at next batch
	uint16_t window;
};

static bool s_matches (coinc_vec_t* cvec, coinc_vec_t* pattern);
static int s_publish_subsc (task_t* self,
	struct s_subscription_t* subsc);

/* -------------------------------------------------------------- */
/* --------------------------- HELPERS -------------------------- */
/* -------------------------------------------------------------- */

static bool
s_matches (coinc_vec_t* cvec, coinc_vec_t* pattern)
{
	for (int i = 0; i < TES_NCHANNELS; i++)
	{
		uint8_t v = (*cvec)[i];
		uint8_t p = (*pattern)[i];
		logmsg (0, LOG_DEBUG + DBG_LETS_GET_NUTS,
			" Count 0x%02hhx vs token 0x%02hhx", v, p);

		if (p == TOK_ANY)
			continue;
		else if (v == TES_COINC_TOK_UNKNOWN)
			return false;
		else if (v > 0 && v <= TES_COINC_MAX_PHOTONS)
		{
			if (p != TOK_NUM && (p ^ v) != 0)
				return false;
		}
		else if (v == 0 || v == TES_COINC_TOK_NOISE)
		{
			if (p != 0 && (p ^ v) != 0)
				return false;
		}
		else
			assert (false);
	}
	return true;
}

static int
s_publish_subsc (task_t* self, struct s_subscription_t* subsc)
{
	dbg_assert (self != NULL);
	dbg_assert (self->data != NULL);
	dbg_assert (subsc != NULL);
	dbg_assert (subsc->active);
	logmsg (0, LOG_DEBUG + DBG_FEELING_LUCKY,
		"Publishing subscription '%s' with %u ticks and "
		"%lu/%lu matches",
		subsc->pattern_str,
		subsc->counts.ticks,
		subsc->counts.num_res_match,
		subsc->counts.num_res);

	struct s_data_t* data = (struct s_data_t*) self->data;

	int rc = zsock_send (self->endpoints[ENDP_PUB].sock,
		TES_COINCCOUNT_PUB_PIC,
		subsc->pattern_str,
		data->window,
		subsc->counts.ticks,
		subsc->counts.num_res_match,
		subsc->counts.num_res_match_noMP,
		subsc->counts.num_res,
		subsc->counts.num_res_noMP,
		subsc->counts.num_unres);
	if (rc == -1)
	{
		logmsg (errno, LOG_ERR, "Cannot send the counts");
		return TASK_ERROR;
	}

	memset (&subsc->counts, 0, sizeof (subsc->counts));
	subsc->counts.ticks =
		(subsc->ticks > 0 ? subsc->ticks : data->ticks);

	return 0;
}

/* -------------------------------------------------------------- */
/* ----------------------------- API ---------------------------- */
/* -------------------------------------------------------------- */

int
task_coinccount_req_hn (zloop_t* loop, zsock_t* endpoint, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;
	assert (self->data != NULL);
	struct s_data_t* data = (struct s_data_t*) self->data;
	
	uint32_t ticks;
	int rc = zsock_recv (endpoint, TES_COINCCOUNT_REQ_PIC, &ticks);
	/* Would also return -1 if picture contained a pointer (p) or a null
	 * frame (z) but message received did not match this signature; this
	 * is irrelevant in this case; we don't get interrupted, this should
	 * not happen. */
	assert (rc != -1);

	if (ticks > 0)
	{
		data->next_ticks = ticks;
		rc = task_conf (self, &ticks, sizeof (ticks), TES_TASK_SAVE_CONF);
		// if ((size_t)rc != sizeof (ticks))
		//   logmsg (errno, LOG_WARNING, "Could not save configuration");
		zsock_send (endpoint, TES_COINCCOUNT_REP_PIC, ticks);
	}
	else
		zsock_send (endpoint, TES_COINCCOUNT_REP_PIC, data->ticks);

	return 0;
}

int
task_coinccount_pub_hn (zloop_t* loop, zsock_t* endpoint, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;
	dbg_assert (self->data != NULL);

	struct s_data_t* data = (struct s_data_t*) self->data;
	dbg_assert (data->next_ticks > 0);

	unsigned char* buf;
	size_t len;
	int rc = zsock_recv (endpoint, "b", &buf, &len);
	assert (rc != -1);

	logmsg (0, LOG_DEBUG + DBG_FEELING_LUCKY,
		"--------------------");
	logmsg (0, LOG_DEBUG + DBG_FEELING_LUCKY,
		"Received %lu bytes from publisher", len);
#if DEBUG_LEVEL > NO_DEBUG
	if (buf == NULL)
	{
		logmsg (0, LOG_ERR, "Received (null) from publisher");
		return TASK_ERROR;
	}
	else if (len % CVEC_SIZE != 0)
		return TASK_ERROR;
#else
	assert (buf != NULL);
	assert (len % CVEC_SIZE == 0);
#endif

	/* Configuration, including window, should not change while there
	 * are active subscriptions, we don't try to detect change. */
	struct task_coinc_hdr_t* hdr = (struct task_coinc_hdr_t*)buf;
	data->window = hdr->window;
	data->cur_ticks += hdr->ticks;
	logmsg (0, LOG_DEBUG + DBG_LETS_GET_NUTS,
		"%u/%u ticks",
		data->cur_ticks, data->ticks);

	bool next_global = (data->ticks >= data->cur_ticks);
	if (next_global || (hdr->ticks > 0 && data->ticks == 0))
	{ /* waiting patterns using global sync will join now */
		data->ticks = data->next_ticks;
		data->cur_ticks = 0;
	}

	if (data->ticks == 0)
	{
		dbg_assert (hdr->ticks == 0);
		dbg_assert (data->cur_ticks == 0);
		return 0; /* no ticks since activation */
	}
	dbg_assert (data->cur_ticks < data->ticks);
	dbg_assert (data->ticks > 0);

	for (struct s_subscription_t* subsc = FIRST_SUB(self);
		subsc != NULL; subsc = NEXT_SUB(self))
	{
		dbg_assert (subsc->counts.num_res_match_noMP <=
			subsc->counts.num_res_match);
		dbg_assert (subsc->counts.num_res_match <=
			subsc->counts.num_res);
		dbg_assert (subsc->counts.num_res_noMP <=
			subsc->counts.num_res);
		dbg_assert (subsc->counts.num_res_match_noMP <=
			subsc->counts.num_res_noMP);
		dbg_assert (subsc->counts.ticks == 0 ||
			subsc->counts.cur_ticks < subsc->counts.ticks);
		dbg_assert (subsc->active || subsc->counts.num_res == 0);
		dbg_assert (subsc->active || subsc->counts.num_unres == 0);
		dbg_assert (subsc->active || subsc->counts.ticks == 0);

		if ( ! subsc->active )
		{
			dbg_assert (subsc->counts.num_res == 0);
			dbg_assert (subsc->counts.num_unres == 0);
			dbg_assert (subsc->counts.ticks == 0);

			subsc->active = (hdr->ticks > 0 &&
				(next_global || subsc->is_private));
			if ( ! subsc->active )
				continue;

			logmsg (0, LOG_DEBUG + DBG_FEELING_LUCKY,
				"Activating subscription '%s'", subsc->pattern_str);
			subsc->counts.ticks =
				(subsc->ticks > 0 ? subsc->ticks : data->ticks);
		}
		else
		{ /* 'else' so we don't count this batch of ticks for patterns
				 that just joined */
			subsc->counts.cur_ticks += hdr->ticks;
			if (subsc->counts.cur_ticks >= subsc->counts.ticks &&
					s_publish_subsc (self, subsc) == TASK_ERROR)
				return TASK_ERROR;
		}

		for (size_t pos = TES_COINC_HDR_LEN; pos < len; pos += CVEC_SIZE)
		{
			dbg_assert (pos + CVEC_SIZE <= len);
			coinc_vec_t* cvec = (coinc_vec_t*)(buf + pos);
			bool mp = (*cvec)[0] & TES_COINC_VEC_FLAG_BAD;
			bool unres = (*cvec)[0] & TES_COINC_VEC_FLAG_UNRESOLVED;
			logmsg (0, LOG_DEBUG + DBG_LETS_GET_NUTS,
				"Vec: unresolved: %s, MP: %s",
				unres ? "yes" : "no", mp ? "yes" : "no");

			if (unres)
			{
				subsc->counts.num_unres++;
				return 0;
			}
			subsc->counts.num_res++;
			if ( ! mp )
				subsc->counts.num_res_noMP++;

			if (s_matches (cvec, &subsc->pattern))
			{
				logmsg (0, LOG_DEBUG + DBG_LETS_GET_NUTS, "  MATCH ");
				subsc->counts.num_res_match++;
				if ( ! mp )
					subsc->counts.num_res_match_noMP++;
			}
		}
	}
	freen (buf); /* from czmq_prelude */
	return 0;
}

int
task_coinccount_init (task_t* self)
{
	assert (self != NULL);
	assert (self->endpoints[ENDP_PUB].type == ZMQ_XPUB);
	assert (self->endpoints[ENDP_SUB].type == ZMQ_SUB);
	assert (self->endpoints[ENDP_REP].type == ZMQ_REP);
	assert ((TOK_ANY & ~TES_COINC_FLAG_MASK) != TES_COINC_TOK_NONE);
	assert ((TOK_ANY & ~TES_COINC_FLAG_MASK) != TES_COINC_TOK_NOISE);
	assert ((TOK_ANY & ~TES_COINC_FLAG_MASK) != TES_COINC_TOK_UNKNOWN);
	assert ((TOK_NUM & ~TES_COINC_FLAG_MASK) != TES_COINC_TOK_NONE);
	assert ((TOK_NUM & ~TES_COINC_FLAG_MASK) != TES_COINC_TOK_NOISE);
	assert ((TOK_NUM & ~TES_COINC_FLAG_MASK) != TES_COINC_TOK_UNKNOWN);

	static struct s_data_t data;
	self->data = &data;

	bool set_default = true;
	ssize_t rc = task_conf (self, &data.next_ticks,
		sizeof (data.next_ticks), TES_TASK_READ_CONF);
	if ((size_t)rc == sizeof (data.next_ticks))
	{
		if (data.next_ticks == 0)
			logmsg (0, LOG_WARNING, "Read invalid configuration");
		else
			set_default = true;
	}

	if (set_default)
		data.next_ticks = 1;

	return 0;
}

int
task_coinccount_wakeup (task_t* self)
{
	assert (self != NULL);
	assert (self->data != NULL);

	endp_subscribe (&self->endpoints[ENDP_SUB], "");

	struct s_data_t* data = (struct s_data_t*) self->data;
	data->ticks = 0; /* wait for first tick */
	data->cur_ticks = 0;
	return 0;
}

int
task_coinccount_sleep (task_t* self)
{
	assert (self != NULL);
	assert (self->data != NULL);

	endp_unsubscribe (&self->endpoints[ENDP_SUB], "");
	return 0;
}

int
task_coinccount_sub_process (const char* pattern_str, void** subsc_p)
{
	assert (pattern_str != NULL);
	assert (subsc_p != NULL);
	*subsc_p = NULL;

	if (strlen (pattern_str) >= MAX_SUBSC_LEN)
	{
		logmsg (0, LOG_DEBUG, "Subscription pattern too long");
		return 0;
	}

	struct s_subscription_t subsc = {0};
	
	unsigned int tok = 0;
	bool symbolic = true;
	int ntoks = 0;
	const char* p = pattern_str;
	for (; *p != '\0'; p++)
	{
		if (ntoks == TES_NCHANNELS)
		{
			logmsg (0, LOG_DEBUG, "Too many tokens");
			return 0;
		}

		if (*p == TES_COINCCOUNT_SEP_SYM)
		{
			if (symbolic && tok == 0)
				tok = TOK_ANY;
			logmsg (0, LOG_DEBUG + DBG_VERBOSE,
				"Token is 0x%02hhx", tok);
			subsc.pattern[ntoks] = tok;
			ntoks++;
			tok = 0;
			symbolic = true;
			continue;
		}

		if (*p > 47 && *p < 58)
		{ /* ASCII 0 to 9 */
			if (symbolic && tok != 0)
			{
				logmsg (0, LOG_DEBUG, "Extra digits after symbols");
				return 0;
			}
			symbolic = false;

			if (tok != 0)
				tok *= 10;
			tok += (*p) - 48;
			if (tok > TES_COINC_MAX_PHOTONS)
			{
				logmsg (0, LOG_DEBUG, "Invalid number");
				return 0;
			}
			continue;
		}

		if ( ! symbolic )
		{
			logmsg (0, LOG_DEBUG, "Extra symbols after digits");
			return 0;
		}
		if (tok != 0)
		{
			logmsg (0, LOG_DEBUG,
				"Symbolic tokens must be a single character");
			return 0;
		}

		switch (*p)
		{
			case TES_COINCCOUNT_SYM_NOISE:
				tok = TES_COINC_TOK_NOISE;
				break;
			case TES_COINCCOUNT_SYM_NUM:
				tok = TOK_NUM;
				break;
			case TES_COINCCOUNT_SYM_ANY:
				tok = TOK_ANY;
				break;
			default:
				logmsg (0, LOG_DEBUG, "Invalid token");
				return 0;
		}
	}
	/* Add the token following the last separator (or the start of the
	 * string if no separator. */
	if (ntoks == TES_NCHANNELS)
	{
		logmsg (0, LOG_DEBUG, "Too many tokens");
		return 0;
	}
	if (symbolic && tok == 0)
		tok = TOK_ANY; /* nothing after last separator */
	else
		logmsg (0, LOG_DEBUG + DBG_VERBOSE,
			"Token is 0x%02hhx", tok);
	subsc.pattern[ntoks] = tok;
	ntoks++;

	/* Add missing trailing 'X's. */
	for (; ntoks < TES_NCHANNELS; ntoks++)
		subsc.pattern[ntoks] = TOK_ANY;

	/* Read tick number. */
	char* buf;
	if (*p == TES_COINCCOUNT_SEP_TICKS)
	{
		long long int ticks = -1;
		ticks = strtoll (p + 1, &buf, 10);
		if (strlen (buf) > 0 || ticks < 0)
		{
			logmsg (0, LOG_DEBUG, "Invalid tick number");
			return 0;
		}
		logmsg (0, LOG_DEBUG, "Using own tick counter: %lld", ticks);
		subsc.is_private = true; /* start at next tick */
		subsc.ticks = (uint32_t) ticks;
	}

	/* Save pattern string. */
	int rc = snprintf (subsc.pattern_str, MAX_SUBSC_LEN, "%s",
		pattern_str);
	assert (rc < MAX_SUBSC_LEN);

	*subsc_p = malloc (sizeof (struct s_subscription_t));
	if (*subsc_p == NULL)
	{
		logmsg (0, LOG_ERR, "Out of memory");
		return TASK_ERROR;
	}
	memcpy (*subsc_p, &subsc, sizeof (struct s_subscription_t));

	logmsg (0, LOG_DEBUG, "Added subscription '%s'",
		pattern_str);
	return 0;
}
