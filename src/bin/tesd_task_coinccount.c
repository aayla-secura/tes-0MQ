/*
 * TODO:
 *  - per subscriber tick counter
 *  - set window from config
 *  - check pattern agains no. set thresholds
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
		uint64_t ticks; // TODO
		uint64_t cur_ticks;
	} counts;
	bool publishing; // wait for next round of published counts
	                 // to synchronize displays
};

struct s_data_t
{
	uint64_t ticks;
	uint64_t cur_ticks;
	uint64_t next_ticks;
	uint16_t window;
};

static bool s_matches (coinc_vec_t* cvec, coinc_vec_t* pattern);
static int s_publish (task_t* self);

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
s_publish (task_t* self)
{
	dbg_assert (self != NULL);
	struct s_data_t* data = (struct s_data_t*) self->data;
	dbg_assert (data->ticks == data->cur_ticks);

	for (struct s_subscription_t* subsc = FIRST_SUB(self);
		subsc != NULL; subsc = NEXT_SUB(self))
	{
		if ( ! subsc->publishing )
		{
			subsc->publishing = true;
			continue;
		}
		int rc = zsock_send (self->endpoints[ENDP_PUB].sock,
			TES_COINCCOUNT_PUB_PIC,
			subsc->pattern_str,
			data->window,
			data->ticks,
			subsc->counts.num_res_match,
			subsc->counts.num_res_match_noMP,
			subsc->counts.num_res,
			subsc->counts.num_res_noMP,
			subsc->counts.num_unres);
		if (rc == -1)
		{
			logmsg (errno, LOG_ERR,
				"Cannot send the counts");
			return TASK_ERROR;
		}
		memset (&subsc->counts, 0, sizeof (subsc->counts));
	}
	data->ticks = data->next_ticks;
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
	struct s_data_t* data = (struct s_data_t*) self->data;
	
	uint64_t ticks;
	int rc = zsock_recv (endpoint, TES_COINCCOUNT_REQ_PIC, &ticks);
	/* Would also return -1 if picture contained a pointer (p) or a null
	 * frame (z) but message received did not match this signature; this
	 * is irrelevant in this case; we don't get interrupted, this should
	 * not happen. */
	assert (rc != -1);

	if (ticks > 0)
	{
		data->next_ticks = ticks;
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
	struct s_data_t* data = (struct s_data_t*) self->data;

	coinc_vec_t* cvec;
	size_t len;
	int rc = zsock_recv (endpoint, "b", &cvec, &len);
	assert (rc != -1);
#if DEBUG_LEVEL >= TESTING
	if (cvec == NULL)
	{
		logmsg (0, LOG_ERR, "Received (null) from publisher");
		return TASK_ERROR;
	}
	else if (len != CVEC_SIZE)
	{
		logmsg (0, LOG_ERR,
			"Received only %lu bytes from publisher", len);
		return TASK_ERROR;
	}
#else
	assert (cvec != NULL);
	assert (len == CVEC_SIZE);
#endif

	/* Check if it's a tick vector (no counts). */
	bool is_tick = true;
	for (int i = 0; i < TES_NCHANNELS; i++)
	{
		if (((*cvec)[i] & ~TES_COINC_FLAG_MASK) != TES_COINC_TOK_TICK)
		{
			is_tick = false;
			break;
		}
	}
	bool has_counts = ! is_tick;

#if TICK_WITH_COINC > 0
	/* Check if it includes a tick. */
	if (has_counts && ((*cvec)[0] & TES_COINC_FLAG_TICK))
		is_tick = true; /* has_counts stays true */
#endif
	if (is_tick)
	{
		data->cur_ticks++;
		if (data->cur_ticks == data->ticks &&
			s_publish (self) == TASK_ERROR)
			return TASK_ERROR;
	}

	if ( ! has_counts )
		return 0;

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

		if ( ! subsc->publishing )
		{
			dbg_assert (subsc->counts.num_res == 0);
			dbg_assert (subsc->counts.num_unres == 0);
			continue;
		}

		if ((*cvec)[0] & TES_COINC_FLAG_UNRESOLVED)
		{
			subsc->counts.num_unres++;
			continue;
		}
		subsc->counts.num_res++;
		bool mp = (*cvec)[0] & TES_COINC_FLAG_BAD;
		if ( ! mp )
			subsc->counts.num_res_noMP++;

		if (s_matches (cvec, &subsc->pattern))
		{
			subsc->counts.num_res_match++;
			if ( ! mp )
				subsc->counts.num_res_match_noMP++;
		}
	}

	freen (cvec); /* from czmq_prelude */
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
	return 0;
}

int
task_coinccount_wakeup (task_t* self)
{
	assert (self != NULL);

	endp_subscribe (&self->endpoints[ENDP_SUB], "");
	return 0;
}

int
task_coinccount_sleep (task_t* self)
{
	assert (self != NULL);

	endp_unsubscribe (&self->endpoints[ENDP_SUB], "");
	return 0;
}

void*
task_coinccount_sub_process (const char* pattern_str)
{
	assert (pattern_str != NULL);
	if (strlen (pattern_str) >= MAX_SUBSC_LEN)
	{
		logmsg (0, LOG_DEBUG, "Subscription pattern too long");
		return NULL;
	}

	struct s_subscription_t subsc = {0};
	
	unsigned int tok = 0;
	bool symbolic = true;
	int ntoks = 0;
	for (const char* p = pattern_str; *p != '\0'; p++)
	{
		if (ntoks == TES_NCHANNELS)
		{
			logmsg (0, LOG_DEBUG, "Too many tokens");
			return NULL;
		}

		if (*p == TES_COINCCOUNT_SEPARATOR)
		{
			if (symbolic && tok == 0)
				tok = TES_COINCCOUNT_SYM_ANY;
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
				return NULL;
			}
			symbolic = false;

			if (tok != 0)
				tok *= 10;
			tok += (*p) - 48;
			if (tok > TES_COINC_MAX_PHOTONS)
			{
				logmsg (0, LOG_DEBUG, "Invalid number");
				return NULL;
			}
			continue;
		}

		if ( ! symbolic )
		{
			logmsg (0, LOG_DEBUG, "Extra symbols after digits");
			return NULL;
		}
		if (tok != 0)
		{
			logmsg (0, LOG_DEBUG,
				"Symbolic tokens must be a single character");
			return NULL;
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
				return NULL;
		}
	}
	/* Add the token following the last separator (or the start of the
	 * string if no separator. */
	if (ntoks == TES_NCHANNELS)
	{
		logmsg (0, LOG_DEBUG, "Too many tokens");
		return NULL;
	}
	if (symbolic && tok == 0)
		tok = TES_COINCCOUNT_SYM_ANY; /* nothing after last separator */
	subsc.pattern[ntoks] = tok;
	ntoks++;

	/* Add missing trailing 'X's */
	for (; ntoks < TES_NCHANNELS; ntoks++)
		subsc.pattern[ntoks] = TES_COINCCOUNT_SYM_ANY;

	int rc = snprintf (subsc.pattern_str, MAX_SUBSC_LEN, "%s",
		pattern_str);
	assert (rc < MAX_SUBSC_LEN);

	struct s_subscription_t* subsc_p = (struct s_subscription_t*)
		malloc (sizeof (struct s_subscription_t));
	if (subsc_p == NULL)
	{
		logmsg (0, LOG_ERR, "Out of memory");
		/* FIXME: How to propagate error here... */
		return NULL;
	}
	memcpy (subsc_p, &subsc, sizeof (struct s_subscription_t));

	logmsg (0, LOG_DEBUG, "Added subscription '%s'",
		pattern_str);
	return (void*)subsc_p;
}
