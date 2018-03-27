#include "tesd_tasks.h"

/*
 * Data for currently built histogram.
 */
struct s_data_t
{
#if DEBUG_LEVEL >= VERBOSE
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
	hist->discard = false;
}

/* -------------------------------------------------------------- */
/* ----------------------------- API ---------------------------- */
/* -------------------------------------------------------------- */

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
			hist->discard = true;
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
			hist->discard = true;
		}

		if (hist->discard)
		{
			/* Drop the previous one. */
			s_clear (hist);
#if DEBUG_LEVEL >= VERBOSE
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
		hist->discard = true;
		return 0;
	}

	/* Copy frame, check current size. */
	uint16_t paylen = flen - TESPKT_HDR_LEN;
	dbg_assert (hist->cur_size <= TES_HIST_MAXSIZE - paylen);
	memcpy (hist->buf + hist->cur_size,
		(char*)pkt + TESPKT_HDR_LEN, paylen);

	hist->cur_size += paylen;

	if (hist->cur_nbins == hist->nbins) 
	{
		dbg_assert (hist->cur_size == hist->size);

		/* Send the histogram */
		int rc = zmq_send (
			zsock_resolve (self->frontends[0].sock),
			hist->buf, hist->cur_size, 0);
		if (rc == -1)
		{
			logmsg (errno, LOG_ERR,
				"Cannot send the histogram");
			return TASK_ERROR;
		}

#if DEBUG_LEVEL >= VERBOSE
		if ((unsigned int)rc != hist->cur_size)
			logmsg (errno, LOG_ERR,
				"Histogram is %lu bytes long, sent %u",
				hist->cur_size, rc);

		hist->published++;
		if (hist->published % 50 == 0)
			logmsg (0, LOG_DEBUG,
				"Published 50 more histogtams");
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
	hist.discard = true;

	self->data = &hist;
	return 0;
}

int
task_hist_wakeup (task_t* self)
{
	assert (self != NULL);
	struct s_data_t* hist = (struct s_data_t*) self->data;

	s_clear (hist);
	hist->discard = true;
	return 0;
}

int
task_hist_fin (task_t* self)
{
	assert (self != NULL);

	self->data = NULL;
	return 0;
}
