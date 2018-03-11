#include "tesd_tasks.h"

/*
 * Data for currently built histogram.
 */
struct s_data_t
{
#ifdef ENABLE_FULL_DEBUG
	uint64_t      published; // number of published histograms
	uint64_t      dropped;   // number of aborted histograms
#endif
	uint16_t      nbins;     // total number of bins in histogram
	uint16_t      cur_nbins; // number of received bins so far
#ifndef TES_MCASIZE_BUG
	uint16_t      size;      // size of histogram including header
	uint16_t      cur_size;  // number of received bytes so far
#else
	uint32_t      size;      // size of histogram including header
	uint32_t      cur_size;  // number of received bytes so far
#endif
	uint32_t      nsubs;     // no. of subscribers at any time
	bool          discard;   // discard all frames until next header
	unsigned char buf[TES_HIST_MAXSIZE];
};

static void s_clear (struct s_data_t* hist);

/* -------------------------------------------------------------- */
/* --------------------------- HELPERS -------------------------- */
/* -------------------------------------------------------------- */

static void
s_clear (struct s_data_t* hist)
{
	dbg_assert (hist != NULL);

	hist->nbins = 0;
	hist->cur_nbins = 0;
	hist->size = 0;
	hist->cur_size = 0;
	hist->discard = 0;
}

/* -------------------------------------------------------------- */
/* ----------------------------- API ---------------------------- */
/* -------------------------------------------------------------- */

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
task_hist_sub_hn (zloop_t* loop, zsock_t* frontend, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;

	zmsg_t* msg = zmsg_recv (frontend);
	/* We don't get interrupted, this should not happen. */
	assert (msg != NULL);

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
		s_clear (hist);
		hist->discard = 1;
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
 * Accumulates MCA frames and sends them out as soon as the last one
 * is received. It aborts the whole histogram if an MCA frame is
 * lost or if extra frames are received (i.e. the size field appears
 * to small).
 */
int
task_hist_pkt_hn (zloop_t* loop, tespkt* pkt, uint16_t flen,
		uint16_t missed, int err, task_t* self)
{
	dbg_assert (self != NULL);

	if (err)
		return 0; /* we don't handle bad frames */

	if ( ! tespkt_is_mca (pkt) )
		return 0;

	struct s_data_t* hist = (struct s_data_t*) self->data;

	if ( ! tespkt_is_header (pkt) )
	{
		if (hist->discard) 
			return 0;

		/* Check protocol sequence */
		uint16_t cur_pseq = tespkt_pseq (pkt);
		if ((uint16_t)(cur_pseq - self->prev_pseq_mca) != 1)
		{
			logmsg (0, LOG_INFO,
				"Frame out of protocol sequence: %hu -> %hu",
				self->prev_pseq_mca, cur_pseq);
			hist->discard = 1;
			return 0;
		}
	}
	else
	{
		if (hist->cur_nbins > 0)
		{
			logmsg (0, LOG_WARNING,
				"Received new header frame while waiting for "
				"%d more bins", hist->nbins - hist->cur_nbins);
			hist->discard = 1;
		}

		if (hist->discard)
		{
			/* Drop the previous one. */
			s_clear (hist);
#ifdef ENABLE_FULL_DEBUG
			hist->dropped++;
			logmsg (0, LOG_DEBUG,
				"Discarded %lu out of %lu histograms so far",
				hist->dropped, hist->dropped + hist->published);
#endif
		}

		dbg_assert (hist->nbins == 0);
		dbg_assert (hist->size == 0);
		dbg_assert (hist->cur_nbins == 0);
		dbg_assert (hist->cur_size == 0);
		dbg_assert ( ! hist->discard );

		/* Inspect header */
		hist->nbins = tespkt_mca_nbins_tot (pkt);
		hist->size  = tespkt_mca_size (pkt);
	}
	dbg_assert ( ! hist->discard );

	hist->cur_nbins += tespkt_mca_nbins (pkt);
	if (hist->cur_nbins > hist->nbins)
	{
		logmsg (0, LOG_WARNING,
			"Received extra bins: expected %d, so far got %d",
			hist->nbins, hist->cur_nbins);
		hist->discard = 1;
		return 0;
	}

	/* Copy frame, check current size. */
	uint16_t paylen = flen - TES_HDR_LEN;
	dbg_assert (hist->cur_size <= TES_HIST_MAXSIZE - paylen);
	memcpy (hist->buf + hist->cur_size,
		(char*)pkt + TES_HDR_LEN, paylen);

	hist->cur_size += paylen;

	if (hist->cur_nbins == hist->nbins) 
	{
		dbg_assert (hist->cur_size == hist->size);

		/* Send the histogram */
#ifdef ENABLE_FULL_DEBUG
		hist->published++;
		int rc = zmq_send (
			zsock_resolve (self->frontends[0].sock),
			hist->buf, hist->cur_size, 0);
		if (rc == -1)
		{
			logmsg (errno, LOG_ERR,
				"Cannot send the histogram");
			return TASK_ERROR;
		}
		if ((unsigned int)rc != hist->cur_size)
		{
			logmsg (errno, LOG_ERR,
				"Histogram is %lu bytes long, sent %u",
				hist->cur_size, rc);
			return TASK_ERROR;
		}
		if (hist->published % 50)
			logmsg (0, LOG_DEBUG,
				"Published 50 more histogtams");
#else
		zmq_send (zsock_resolve (self->frontends[0].sock),
			hist->buf, hist->cur_size, 0);
#endif

		s_clear (hist);
		return 0;
	}

	dbg_assert (hist->cur_size < hist->size);
	return 0;
}

int
task_hist_init (task_t* self)
{
	assert (self != NULL);

	static struct s_data_t hist;
	hist.discard = 1;

	self->data = &hist;
	return 0;
}

int
task_hist_fin (task_t* self)
{
	assert (self != NULL);

	self->data = NULL;
	return 0;
}
