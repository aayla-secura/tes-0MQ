#include "tesd_tasks.h"
#include <aio.h>
#include "hdf5conv.h"

/* See README */
#define TSAVE_REQ_OK    0 // accepted
#define TSAVE_REQ_INV   1 // malformed request
#define TSAVE_REQ_ABORT 2 // no such job (for status query) or file exist (for no-overwrite)
#define TSAVE_REQ_EPERM 3 // filename is not allowed
#define TSAVE_REQ_FAIL  4 // other error opening the file, nothing was written
#define TSAVE_REQ_EWRT  5 // error while writing, less than minimum requested was saved
#define TSAVE_REQ_ECONV 6 // error while converting to hdf5
#define TSAVE_REQ_PIC   "ss8811"
#define TSAVE_REP_PIC "18888888"

#define TSAVE_FIDX_LEN 16         // frame index
#define TSAVE_TIDX_LEN  8         // tick index
#define TSAVE_SIDX_LEN 16         // MCA and trace indices
#define TSAVE_STAT_LEN 64         // job statistics
#define TSAVE_ROOT "/media/data/" // must have a trailing slash

#define TSAVE_REQUIRE_FILENAME    // for now we don't generate filenames
// #define TSAVE_SINGLE_FILE         // save all payloads (with headers) to single .dat file
#ifdef TSAVE_SINGLE_FILE
#  ifndef TSAVE_SAVE_HEADERS
#    define TSAVE_SAVE_HEADERS
#  endif
#endif
// #define TSAVE_SAVE_HEADERS        // save headers in .*dat files
// #define TSAVE_NO_BAD_FRAMES       // drop bad frames

/* Employ a buffer zone for asynchronous writing. We memcpy frames into the
 * bufzone, between its head and cursor (see s_task_save_data_t below) and
 * queue batches with aio_write. aio_write has significant overhead and is not
 * worth queueing less than ~2kB (it'd be much slower than synchronous write).
 * */
#define TSAVE_BUFSIZE 10485760UL // 10 MB
#define TSAVE_MINSIZE 512000UL   // 500 kB
#ifdef ENABLE_FULL_DEBUG
#  define TSAVE_HISTBINS 11
#endif

/*
 * Transformed packet type byte: for the frame index
 */
struct s_task_save_ftype_t
{
	/* PT: */
#define TSAVE_FTYPE_PEAK        0
#define TSAVE_FTYPE_AREA        1
#define TSAVE_FTYPE_PULSE       2
#define TSAVE_FTYPE_TRACE_SGL   3
#define TSAVE_FTYPE_TRACE_AVG   4
#define TSAVE_FTYPE_TRACE_DP    5
#define TSAVE_FTYPE_TRACE_DP_TR 6
#define TSAVE_FTYPE_TICK        7
#define TSAVE_FTYPE_MCA         8
#define TSAVE_FTYPE_BAD         9
	uint8_t PT  : 4;
	uint8_t     : 3; /* reserved */
	uint8_t SEQ : 1; /* sequence error in event stream */
};
#define linear_etype(pkt_type,tr_type) \
	( (pkt_type == PKT_TYPE_TRACE) ? 3 + tr_type : pkt_type )

/*
 * Statistics sent as a reply and saved to the file. 
 */
struct s_task_save_stats_t
{
	uint64_t ticks;
	uint64_t events;         // number of events written 
	uint64_t traces;         // number of traces written 
	uint64_t hists;          // number of histograms written 
	uint64_t frames;         // total frames saved
	uint64_t frames_lost;    // total frames lost
	uint64_t frames_dropped; // total frames dropped
	uint64_t errors;         // TO DO: last 8-bytes of the tick header 
};

/*
 * A list of stream and index files.
 */
#ifdef TSAVE_SINGLE_FILE
#  define TSAVE_NUM_DSETS 5
#else
#  define TSAVE_NUM_DSETS 8
#endif
static struct s_task_save_dset_t
{
	char* dataset;   // name of dataset inside hdf5 file
	char* extension; // file extension
} s_task_save_dsets[] = {
#  define TSAVE_DSET_FIDX 0
	{ // frame index
		.dataset = "fidx",
		.extension = "fidx",
	},
#  define TSAVE_DSET_MIDX 1
	{ // MCA index
		.dataset = "midx",
		.extension = "midx",
	},
#  define TSAVE_DSET_TIDX 2
	{ // tick index
		.dataset = "tidx",
		.extension = "tidx",
	},
#  define TSAVE_DSET_RIDX 3
	{ // trace index
		.dataset = "ridx",
		.extension = "ridx",
	},
#ifdef TSAVE_SINGLE_FILE
#  define TSAVE_DSET_ADAT 4
	{ // all payloads
		.dataset = "all data",
		.extension = "adat",
	},
#else
#  define TSAVE_DSET_BDAT 4
	{ // bad payloads
		.dataset = "bad",
		.extension = "bdat",
	},
#  define TSAVE_DSET_MDAT 5
	{ // MCA payloads
		.dataset = "mca",
		.extension = "mdat",
	},
#  define TSAVE_DSET_TDAT 6
	{ // tick payloads
		.dataset = "ticks",
		.extension = "tdat",
	},
#  define TSAVE_DSET_EDAT 7
	{ // event payloads
		.dataset = "events",
		.extension = "edat",
	},
#endif
};

/*
 * Data related to a stream or index file, e.g. ticks or MCA frames.
 */
struct s_task_save_aiobuf_t
{
	struct aiocb aios;
	struct
	{
		unsigned char* base; // mmapped, size of TSAVE_BUFSIZE
		unsigned char* tail; // start address queued for aio_write
		unsigned char* cur;  // address where next packet will be coppied to
		unsigned char* ceil; // base + TSAVE_BUFSIZE
		size_t waiting;      // copied into buffer since the last aio_write
		size_t enqueued;     // queued for writing at the last aio_write
#ifdef ENABLE_FULL_DEBUG
		struct {
			size_t prev_enqueued;
			size_t prev_waiting;
			size_t last_written;
			uint64_t batches[TSAVE_HISTBINS];
			uint64_t failed_batches;
			uint64_t num_skipped;
			uint64_t num_blocked;
		} st;
#endif
	} bufzone;
	size_t size; // number of bytes written
	char   filename[PATH_MAX]; // name data/index file
	char*  dataset;            // name of dataset inside hdf5 file
	                           // points to one of the const
	                           // strings in s_task_save_dsets
};

/*
 * The frame index.
 * Flags mca, bad and seq are set in event type.
 */
struct s_task_save_fidx_t
{
	uint64_t start;   // frame's offset into its corresponding dat file
	uint32_t length;  // payload's length
	uint16_t esize;   // original event size
	uint8_t  changed; // event frame differs from previous
	struct s_task_save_ftype_t ftype; // see definition of struct
};

/*
 * The tick index.
 */
struct s_task_save_tidx_t
{
	uint32_t start_frame; // frame number of first non-tick event
	uint32_t stop_frame;  // frame number of last non-tick event
};

/*
 * The MCA and trace indices. (the 's' is for 'stream')
 */
struct s_task_save_sidx_t
{
	uint64_t start;  // first byte of histogram/trace into dat file
	uint64_t length; // length in bytes of histogram/trace
};

/*
 * Data for the currently-saved file. min_ticks and basefname are set when
 * receiving a request from client.
 */
struct s_task_save_data_t
{
	struct s_task_save_stats_t st;
	struct s_task_save_aiobuf_t aio[TSAVE_NUM_DSETS];

	struct
	{
		struct s_task_save_sidx_t idx;
		size_t size;
		size_t cur_size;
		bool is_event; // i.e. is_trace, otherwise it's MCA
		bool discard;  // stream had errors, ignore rest
	} cur_stream;          // ongoing trace of histogram

	struct
	{
		struct s_task_save_tidx_t idx;
		uint32_t nframes; // no. of event frames in this tick
	} cur_tick;
	uint8_t  prev_esize; // event size for previous event
	uint8_t  prev_etype; // transformed event type for previous event, see PT above

	uint64_t min_ticks;   // cpature at least that many ticks
	uint64_t min_events;  // cpature at least that many events
	uint8_t  overwrite;   // overwrite hdf5 file
	uint8_t  async;       // copy data to hdf5 in the background
	char*    basefname;   // basename for hdf5 file
	char*    measurement; // hdf5 group

	int      fd;          // fd for the statistics file
	bool     recording;   // wait for a tick before starting capture
};

/* Task initializer and finalizer */
static int   s_task_save_init_aiobuf (struct s_task_save_aiobuf_t* aiobuf);
static void  s_task_save_fin_aiobuf (struct s_task_save_aiobuf_t* aiobuf);

/*
 * s_task_save_open and s_task_save_close deal with stream and index files
 * only. stats_* deal with the stats file/database.
 * s_task_save_stats_send should be called at the very end (after
 * processing is done.
 */
/* Job initializer and finalizer */
static int   s_task_save_open  (struct s_task_save_data_t* sjob, mode_t fmode);
static void  s_task_save_close (struct s_task_save_data_t* sjob);
static int   s_task_save_open_aiobuf (struct s_task_save_aiobuf_t* aiobuf,
	mode_t fmode);
static void  s_task_save_close_aiobuf (struct s_task_save_aiobuf_t* aiobuf);
static int   s_task_save_conv_data (struct s_task_save_data_t* sjob);

/* Statistics for a job. */
static int   s_task_save_stats_read  (struct s_task_save_data_t* sjob);
static int   s_task_save_stats_write (struct s_task_save_data_t* sjob);
static int   s_task_save_stats_send  (struct s_task_save_data_t* sjob,
	zsock_t* frontend, uint8_t status);

/* Ongoing job helpers */
static void  s_task_save_flush (struct s_task_save_data_t* sjob);
static int   s_task_save_try_queue_aiobuf (struct s_task_save_aiobuf_t* aiobuf,
	const char* buf, uint16_t len);
static int   s_task_save_queue_aiobuf (struct s_task_save_aiobuf_t* aiobuf,
	bool force);
static char* s_task_save_canonicalize_path (const char* filename,
	bool checkonly);

#ifdef ENABLE_FULL_DEBUG
static void  s_task_save_dbg_stats (struct s_task_save_data_t* sjob);
#endif

/* ------------------------------------------------------------------------- */

/*
 * Called when a client sends a request on the REP socket. For valid requests
 * of status, opens the file and send the reply. For valid requests to save,
 * opens the file and marks the task as active.
 */
int
task_save_req_hn (zloop_t* loop, zsock_t* reader, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;

	struct s_task_save_data_t* sjob =
		(struct s_task_save_data_t*) self->data;
	dbg_assert ( ! sjob->recording );


	char* basefname;
	int rc = zsock_recv (reader, TSAVE_REQ_PIC,
			&basefname,
			&sjob->measurement,
			&sjob->min_ticks,
			&sjob->min_events,
			&sjob->overwrite,
			&sjob->async);
	if (rc == -1)
	{ /* would also return -1 if picture contained a pointer (p) or a null
	   * frame (z) but message received did not match this signature; this
	   * is irrelevant in this case */
		logmsg (0, LOG_DEBUG, "Receive interrupted");
		return TASK_ERROR;
	}

	/* Is the request understood? */
	if (basefname == NULL || sjob->overwrite > 1)
	{
		logmsg (0, LOG_INFO,
			"Received a malformed request");
		zsock_send (reader, TSAVE_REP_PIC, TSAVE_REQ_INV,
				0, 0, 0, 0, 0, 0, 0);
		return 0;
	}

	/* Is it only a status query? */
	bool checkonly = (sjob->min_ticks == 0);
	if (checkonly)
	{
		logmsg (0, LOG_INFO,
			"Received request for status of '%s-%s'",
			basefname, sjob->measurement);
	}
	else
	{
		logmsg (0, LOG_INFO,
			"Received request to write %lu ticks and "
			"%lu events to '%s-%s%s'",
			sjob->min_ticks,
			sjob->min_events,
			basefname,
			sjob->measurement,
			sjob->async ? ". Convering asynchronously" : "");
	}

	/* Check if filename is allowed and get the realpath. */
	sjob->basefname = s_task_save_canonicalize_path (
			basefname, checkonly);
	zstr_free (&basefname); /* nullifies the pointer */

	if (sjob->basefname == NULL)
	{
		if (checkonly)
		{
			logmsg (0, LOG_INFO,
					"Job not found");
			zsock_send (reader, TSAVE_REP_PIC, TSAVE_REQ_ABORT,
					0, 0, 0, 0, 0, 0, 0);
		}
		else
		{
			logmsg (errno, LOG_INFO,
					"Filename is not valid");
			zsock_send (reader, TSAVE_REP_PIC, TSAVE_REQ_EPERM,
					0, 0, 0, 0, 0, 0, 0);
		}

		return 0;
	}

	dbg_assert (sjob->basefname != NULL);

	/*
	 * *****************************************************************
	 * ************************** Status query. ************************
	 * *****************************************************************
	 */
	if (checkonly)
	{ /* just read in stats and send reply */
		rc = s_task_save_stats_read  (sjob);
		if (rc != 0)
		{
			logmsg (errno, LOG_ERR,
				"Could not read stats");
			zsock_send (reader, TSAVE_REP_PIC, TSAVE_REQ_FAIL,
					0, 0, 0, 0, 0, 0, 0);
			return 0;
		}
		rc = s_task_save_stats_send  (sjob, self->frontend, TSAVE_REQ_OK);
		if (rc != 0)
		{
			logmsg (0, LOG_NOTICE,
				"Could not send stats");
		}
		return 0;
	}

	/*
	 * *****************************************************************
	 * ************************* Write request. ************************
	 * *****************************************************************
	 */
	/*
	 * Set the file open mode and act according to the return status of
	 * open and errno (print a warning of errno is unexpected)
	 * Request is for:
	 *   create: create if non-existing
	 *           - if successful, enable save
	 *           - if failed, send reply (expect errno == EEXIST)
	 *   create: create or overwrite
	 *           - if successful, enable save
	 *           - if failed, send reply (this shouldn't happen)
	 */
	int exp_errno = 0;
	mode_t fmode = O_RDWR | O_CREAT;
	if ( ! sjob->overwrite )
	{ /* do not overwrite */
		fmode |= O_EXCL;
		exp_errno = EEXIST;
	}

	rc = s_task_save_open (sjob, fmode);
	if (rc == -1)
	{
		if (errno != exp_errno)
		{
			logmsg (errno, LOG_ERR,
				"Could not open file %s",
				sjob->basefname);
			zsock_send (reader, TSAVE_REP_PIC, TSAVE_REQ_FAIL,
					0, 0, 0, 0, 0, 0, 0);
		}
		else
		{
			logmsg (0, LOG_INFO, "Job will not proceed");
			zsock_send (reader, TSAVE_REP_PIC, TSAVE_REQ_ABORT,
					0, 0, 0, 0, 0, 0, 0);
		}
		s_task_save_close (sjob);
		return 0;
	}

	logmsg (0, LOG_INFO,
		"Opened files %s-%s.* for writing",
		sjob->basefname, sjob->measurement);

	/* Disable polling on the reader until the job is done. Wakeup packet
	 * handler. */
	task_activate (self);

	return 0;
}

/*
 * Saves packet payloads to corresponding file(s) and writes index files.
 */
int
task_save_pkt_hn (zloop_t* loop, tespkt* pkt, uint16_t flen,
		uint16_t missed, int err, task_t* self)
{
	dbg_assert (self != NULL);

	struct s_task_save_data_t* sjob =
		(struct s_task_save_data_t*) self->data;

	bool is_tick = tespkt_is_tick (pkt);
	if ( ! sjob->recording && is_tick )
		sjob->recording = 1; /* start the capture */

	if ( ! sjob->recording )
		return 0;

	if (err)
	{
#ifdef TSAVE_NO_BAD_FRAMES
		/* drop bad frames */
		sjob->st.frames_dropped++;
		return 0;
#endif
	}

	sjob->st.frames++;
	sjob->st.frames_lost += missed;

	uint16_t esize = tespkt_esize (pkt);
	esize = htofs (esize); /* in FPGA byte-order */
	uint16_t paylen = flen - TES_HDR_LEN;

	bool is_header = tespkt_is_header (pkt);
	bool is_mca = tespkt_is_mca (pkt);
	bool is_trace = ( tespkt_is_trace (pkt) &&
			! tespkt_is_trace_dp (pkt) );

	/* **** Update tick and frame indices and choose the data file. **** */
	struct s_task_save_aiobuf_t* aiofidx = &sjob->aio[TSAVE_DSET_FIDX];
#ifdef TSAVE_SINGLE_FILE
	struct s_task_save_aiobuf_t* aiodat = &sjob->aio[TSAVE_DSET_ADAT];
#else
	struct s_task_save_aiobuf_t* aiodat = NULL; /* set later */
#endif
	struct s_task_save_fidx_t fidx;
	fidx.length = paylen;
	fidx.esize = esize;
	fidx.changed = 0;
	fidx.ftype.SEQ = 0;

	bool finishing = 0;
	int jobrc;

	/* Check for sequence error. */
	if (missed > 0)
	{
#ifdef ENABLE_FULL_DEBUG
#if 0
		logmsg (0, LOG_DEBUG,
			"Missed %hu at frame #%lu",
			missed, sjob->st.frames - 1);
#endif
#endif
		fidx.ftype.SEQ = 1;
	}

	/* Check packet type. */
	if (err)
	{
		fidx.ftype.PT = TSAVE_FTYPE_BAD;
#ifndef TSAVE_SINGLE_FILE
		aiodat = &sjob->aio[TSAVE_DSET_BDAT];
#endif
	}
	else if (is_mca)
	{
		fidx.ftype.PT = TSAVE_FTYPE_MCA;
#ifndef TSAVE_SINGLE_FILE
		aiodat = &sjob->aio[TSAVE_DSET_MDAT];
#endif
	}
	else if (is_tick)
	{
		fidx.ftype.PT = TSAVE_FTYPE_TICK;
#ifndef TSAVE_SINGLE_FILE
		aiodat = &sjob->aio[TSAVE_DSET_TDAT];
#endif

		if (sjob->st.ticks > 0)
		{
			struct s_task_save_tidx_t* tidx = &sjob->cur_tick.idx;
			jobrc = s_task_save_try_queue_aiobuf (
					&sjob->aio[TSAVE_DSET_TIDX], (char*)tidx,
					TSAVE_TIDX_LEN);
			if (jobrc < 0)
				finishing = 1; /* error */
		}

		sjob->cur_tick.nframes = 0;
		/* no need to zero the index */
	}
	else
	{
#ifndef TSAVE_SINGLE_FILE
		aiodat = &sjob->aio[TSAVE_DSET_EDAT];
#endif

		struct s_task_save_tidx_t* tidx = &sjob->cur_tick.idx;
		const struct tespkt_event_type* etype = tespkt_etype (pkt);
		uint8_t pt = linear_etype (etype->PKT, etype->TR);
		fidx.ftype.PT = pt;
		if ( sjob->st.frames > 1 && 
			( sjob->prev_etype != pt || sjob->prev_esize != esize ) )
		{
			fidx.changed = 1;
		}
		sjob->prev_esize = esize;
		sjob->prev_etype = pt;

		if (sjob->cur_tick.nframes == 0)
		{ /* first non-tick event frame after a tick */
			tidx->start_frame = sjob->st.frames - 1;
		}
		else
		{ /* in case it's the last event before a tick */
			tidx->stop_frame = sjob->st.frames - 1;
		}
		sjob->cur_tick.nframes++;
	}

	fidx.start = aiodat->size +
		aiodat->bufzone.waiting + aiodat->bufzone.enqueued;

	/*
	 * *************** Update statistics and stream index. *************
	 * Check if there is an ongoing stream (trace or MCA). If so, update
	 * index if necessary. If this is the last frame of a stream, queue
	 * the index for writing and reset cur_stream's size and cur_size.
	 * size and cur_size would also be reset if an error (e.g. missed
	 * frames) occurs. idx and is_event are set when receiving the header
	 * of a new stream.
	 */

	/* Skip if frame is bad */
	if (err)
		goto done;

	if (sjob->cur_stream.size > 0)
	{
		dbg_assert (sjob->cur_stream.cur_size > 0);
		dbg_assert (sjob->cur_stream.cur_size < sjob->cur_stream.size);
		dbg_assert ( ! sjob->cur_stream.discard );

	}
	else
		dbg_assert (sjob->cur_stream.cur_size == 0);

	bool continues_stream = ( ( (is_trace && sjob->cur_stream.is_event) ||
				    (is_mca && ! sjob->cur_stream.is_event) ) &&
			sjob->cur_stream.size > 0 && ! is_header && missed == 0 );
	bool starts_stream = ( (is_trace || is_mca) && is_header &&
			sjob->cur_stream.size == 0 );
	bool interrupts_stream = ( ! continues_stream &&
			sjob->cur_stream.size > 0 );

	if (interrupts_stream) 
	{ /* unexpected or first missed frames during an ongoing stream */
		sjob->cur_stream.discard = 1;
		sjob->cur_stream.size = 0;
		sjob->cur_stream.cur_size = 0;

		dbg_assert ( is_header || missed > 0 ||
			(is_trace && ! sjob->cur_stream.is_event) ||
			(is_mca && sjob->cur_stream.is_event) ||
			( ! is_trace && ! is_mca ) );
#if 0
		if (missed == 0)
		{ /* should only happen in case of FPGA fault */
			logmsg (0, LOG_NOTICE,
				"Received a%s %sframe (#%lu) "
				"while a %s was ongoing",
				is_mca ? " histogram" : (
					is_trace ? " trace" : (
						is_tick ? " tick" :
						"n event" ) ),
				is_header ? "header " : "",
				sjob->st.frames - 1,
				sjob->cur_stream.is_event ?
				"trace" : "histogram");
		}
#endif
	}

	if (starts_stream || continues_stream)
	{
		if (starts_stream)
		{ /* start a new stream */
			if (is_trace)
			{
				sjob->cur_stream.size = tespkt_trace_size (pkt);
				sjob->cur_stream.is_event = 1;
			}
			else
			{
				sjob->cur_stream.size = tespkt_mca_size (pkt);
				sjob->cur_stream.is_event = 0;
			}
			sjob->cur_stream.discard = 0;

			sjob->cur_stream.idx.start = aiodat->size +
				aiodat->bufzone.waiting + aiodat->bufzone.enqueued;
		}
		else
		{ /* ongoing multi-frame stream */
			dbg_assert ( ! sjob->cur_stream.discard && missed == 0);
		}

		sjob->cur_stream.cur_size += paylen;
		if (sjob->cur_stream.cur_size > sjob->cur_stream.size)
		{ /* extra bytes */
#ifdef ENABLE_FULL_DEBUG
#if 0
			logmsg (0, LOG_DEBUG, "Extra %s data "
					"at frame #%lu",
					is_mca ? "histogram" : "trace",
					sjob->st.frames - 1);
#endif
#endif
			sjob->cur_stream.size = 0;
			sjob->cur_stream.cur_size = 0;
			sjob->cur_stream.discard = 1;
		}
		else if (sjob->cur_stream.cur_size == sjob->cur_stream.size)
		{ /* done, record the event */
			struct s_task_save_aiobuf_t* aiosidx = NULL;
			if (is_trace)
			{
				aiosidx = &sjob->aio[TSAVE_DSET_RIDX];
				sjob->st.events++;
				sjob->st.traces++;
			}
			else
			{
				aiosidx = &sjob->aio[TSAVE_DSET_MIDX];
				sjob->st.hists++;
			}
			sjob->cur_stream.idx.length = sjob->cur_stream.size;
			sjob->cur_stream.size = 0;
			sjob->cur_stream.cur_size = 0;

			jobrc = s_task_save_try_queue_aiobuf (aiosidx,
					(char*)&sjob->cur_stream.idx,
					TSAVE_SIDX_LEN);
			if (jobrc < 0)
				finishing = 1; /* error */
		}
	}
	else if (is_mca || is_trace)
	{ /* missed beginning of a stream or in the process of discarding */
		if ( ! interrupts_stream )
		{
			dbg_assert ( ! is_header );
			dbg_assert (sjob->cur_stream.size == 0);

			if ( ! sjob->cur_stream.discard )
			{
#ifdef ENABLE_FULL_DEBUG
#if 0
				logmsg (0, LOG_DEBUG,
					"Received a non-header %s frame (#%lu) "
					"while no stream was ongoing",
					is_mca ? "histogram" : "trace",
					sjob->st.frames - 1);
#endif
#endif
				sjob->cur_stream.discard = 1;
			}
		}
	}
	else if (is_tick)
	{ /* tick */
		sjob->st.ticks++;
		/* Ticks should be > min_ticks cause we count the
		 * starting one too. */
		if (sjob->st.ticks > sjob->min_ticks &&
				sjob->st.events >= sjob->min_events)
			finishing = 1; /* DONE */
	}
	else
	{ /* short event */
		sjob->st.events += tespkt_event_nums (pkt);
	}

done:
	/* ********************** Write frame payload. ********************* */
#ifdef TSAVE_SAVE_HEADERS
	jobrc = s_task_save_try_queue_aiobuf (aiodat, (char*)pkt,
			flen);
#else
	jobrc = s_task_save_try_queue_aiobuf (aiodat, (char*)pkt + TES_HDR_LEN,
	                 paylen);
#endif
	if (jobrc < 0)
		finishing = 1; /* error */

	/* *********************** Write frame index. ********************** */

	jobrc = s_task_save_try_queue_aiobuf (aiofidx, (char*)&fidx,
			TSAVE_FIDX_LEN);
	if (jobrc < 0)
		finishing = 1; /* error */

	dbg_assert ( sjob->st.frames * TSAVE_FIDX_LEN ==
			aiofidx->size +
			aiofidx->bufzone.waiting +
			aiofidx->bufzone.enqueued );

	/* ********************** Check if done. *************************** */
	if (finishing)
	{
		/* Flush all buffers. */
		s_task_save_flush (sjob);

		logmsg (0, LOG_INFO,
			"Finished writing %lu ticks and %lu events",
			sjob->st.ticks, sjob->st.events);
#ifdef ENABLE_FULL_DEBUG
		s_task_save_dbg_stats (sjob);
#endif
		/* Close stream and index files. */
		s_task_save_close (sjob);

		uint8_t status = ( ( sjob->min_ticks > sjob->st.ticks ||
			 sjob->min_events > sjob->st.events ) ?
			TSAVE_REQ_EWRT : TSAVE_REQ_OK );

		/* Write stats. */
		int rc = s_task_save_stats_write (sjob);
		if (rc != 0)
		{
			status = TSAVE_REQ_EWRT;
			logmsg (errno, LOG_ERR,
				"Could not write stats");
		}

		/* Convert them to hdf5. */
		rc = s_task_save_conv_data (sjob);
		if (rc != 0)
		{
			status = TSAVE_REQ_ECONV;
			logmsg (errno, LOG_ERR,
				"Could not convert data to hdf5");
		}

		/* Send reply. */
		s_task_save_stats_send (sjob, self->frontend, status);

		/* Enable polling on the reader and deactivate packet
		 * handler. */
		return TASK_SLEEP;
	}

	return 0;
}

/*
 * Perform checks and statically allocate the data struct.
 * mmap data for stream and index files.
 * Returns 0 on success, -1 on error.
 */
int
task_save_init (task_t* self)
{
	assert (self != NULL);
	assert (*(TSAVE_ROOT + strlen (TSAVE_ROOT) - 1) == '/');
	assert (sizeof (struct s_task_save_stats_t) == TSAVE_STAT_LEN);
	assert (sizeof (struct s_task_save_fidx_t) == TSAVE_FIDX_LEN);
	assert (sizeof (struct s_task_save_tidx_t) == TSAVE_TIDX_LEN);
	assert (sizeof (struct s_task_save_sidx_t) == TSAVE_SIDX_LEN);
	assert (sizeof (s_task_save_dsets) ==
			TSAVE_NUM_DSETS * sizeof (struct s_task_save_dset_t));
	assert (memcmp (s_task_save_dsets[TSAVE_DSET_FIDX].extension,
				"fidx", 4) == 0);
	assert (memcmp (s_task_save_dsets[TSAVE_DSET_MIDX].extension,
				"midx", 4) == 0);
	assert (memcmp (s_task_save_dsets[TSAVE_DSET_TIDX].extension,
				"tidx", 4) == 0);
	assert (memcmp (s_task_save_dsets[TSAVE_DSET_RIDX].extension,
				"ridx", 4) == 0);
#ifdef TSAVE_SINGLE_FILE
	assert (memcmp (s_task_save_dsets[TSAVE_DSET_ADAT].extension,
				"adat", 4) == 0);
#else
	assert (memcmp (s_task_save_dsets[TSAVE_DSET_BDAT].extension,
				"bdat", 4) == 0);
	assert (memcmp (s_task_save_dsets[TSAVE_DSET_MDAT].extension,
				"mdat", 4) == 0);
	assert (memcmp (s_task_save_dsets[TSAVE_DSET_TDAT].extension,
				"tdat", 4) == 0);
	assert (memcmp (s_task_save_dsets[TSAVE_DSET_EDAT].extension,
				"edat", 4) == 0);
#endif

	static struct s_task_save_data_t sjob;
	sjob.fd = -1;

	int rc = 0;
	for (int s = 0; s < TSAVE_NUM_DSETS ; s++)
	{
		rc = s_task_save_init_aiobuf (&sjob.aio[s]);
		if (rc != 0)
			break;
	}
	if (rc != 0)
	{
		logmsg (errno, LOG_ERR,
			"Cannot mmap %lu bytes", TSAVE_BUFSIZE);
		return -1;
	}

	self->data = &sjob;
	return 0;
}

/*
 * Send off stats for any ongoing job. Close all files.
 * Unmap data for stream and index files.
 * Returns 0 on success, -1 if job status could not be sent or written.
 */
int
task_save_fin (task_t* self)
{
	assert (self != NULL);

	struct s_task_save_data_t* sjob =
		(struct s_task_save_data_t*) self->data;
	assert (sjob != NULL);

	int rc = 0;
	if (sjob->basefname != NULL)
	{ /* A job is in progress. _stats_send nullifies this. */
		s_task_save_flush (sjob);
		s_task_save_close (sjob);
		rc  = s_task_save_stats_write (sjob);
		rc |= s_task_save_stats_send  (sjob,
				self->frontend, TSAVE_REQ_EWRT);
	}

	for (int s = 0; s < TSAVE_NUM_DSETS ; s++)
		s_task_save_fin_aiobuf (&sjob->aio[s]);

	self->data = NULL;
	return (rc ? -1 : 0);
}

/* ------------------------------------------------------------------------- */

/*
 * mmap data for a stream or index file.
 * Returns 0 on success, -1 on error.
 */
static int
s_task_save_init_aiobuf (struct s_task_save_aiobuf_t* aiobuf)
{
	assert (aiobuf != NULL);

	aiobuf->aios.aio_sigevent.sigev_notify = SIGEV_NONE;
	aiobuf->aios.aio_fildes = -1;

	void* buf = mmap (NULL, TSAVE_BUFSIZE, PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == (void*)-1)
		return -1;

	aiobuf->bufzone.base = aiobuf->bufzone.tail =
		aiobuf->bufzone.cur = (unsigned char*) buf;
	aiobuf->bufzone.ceil = aiobuf->bufzone.base + TSAVE_BUFSIZE;

	return 0;
}

/*
 * munmap data for a stream or index file.
 */
static void
s_task_save_fin_aiobuf (struct s_task_save_aiobuf_t* aiobuf)
{
	assert (aiobuf != NULL);

	/* Unmap bufzone */
	if (aiobuf->bufzone.base != NULL)
	{
		munmap (aiobuf->bufzone.base, TSAVE_BUFSIZE);
		aiobuf->bufzone.base = NULL;
	}
}

/*
 * Opens the stream and index files.
 * It does not close any successfully opened files are closed if an error occurs.
 * Returns 0 on success, -1 on error.
 */
static int
s_task_save_open (struct s_task_save_data_t* sjob, mode_t fmode)
{
	assert (sjob != NULL);
	assert (sjob->basefname != NULL);

	dbg_assert (sjob->st.ticks == 0);
	dbg_assert (sjob->st.events == 0);
	dbg_assert (sjob->st.traces == 0);
	dbg_assert (sjob->st.hists == 0);
	dbg_assert (sjob->st.frames == 0);
	dbg_assert (sjob->st.frames_lost == 0);
	dbg_assert (sjob->st.frames_dropped == 0);
	dbg_assert (sjob->st.errors == 0);

	dbg_assert (sjob->cur_stream.size == 0);
	dbg_assert (sjob->cur_stream.cur_size == 0);
	dbg_assert (sjob->cur_tick.nframes == 0);

	/* Open the data files. */
	if (strlen (sjob->basefname) + 6 > PATH_MAX)
	{
		errno = ENAMETOOLONG;
		return -1;
	}

	for (int s = 0; s < TSAVE_NUM_DSETS ; s++)
	{
		struct s_task_save_aiobuf_t* aiobuf = &sjob->aio[s];
		snprintf (aiobuf->filename, PATH_MAX, "%s-%s.%s",
				sjob->basefname,
				sjob->measurement,
				s_task_save_dsets[s].extension);
		aiobuf->dataset = s_task_save_dsets[s].dataset;
		int rc = s_task_save_open_aiobuf (aiobuf, fmode);
		if (rc != 0)
			return -1;
	}

	return 0;
}

/*
 * Closes the stream and index files.
 */
static void
s_task_save_close (struct s_task_save_data_t* sjob)
{
	assert (sjob != NULL);
	assert (sjob->basefname != NULL);

	/* Close the data files. */
	for (int s = 0; s < TSAVE_NUM_DSETS ; s++)
		s_task_save_close_aiobuf (&sjob->aio[s]);
}

/*
 * Open a stream or index file.
 * Returns 0 on success, -1 on error.
 */
static int
s_task_save_open_aiobuf (struct s_task_save_aiobuf_t* aiobuf, mode_t fmode)
{
	assert (aiobuf != NULL);
	assert (aiobuf->filename != NULL);

	dbg_assert (aiobuf->aios.aio_fildes == -1);
	dbg_assert (aiobuf->size == 0);
	dbg_assert (aiobuf->bufzone.cur == aiobuf->bufzone.tail);
	dbg_assert (aiobuf->bufzone.cur == aiobuf->bufzone.base);
	dbg_assert (aiobuf->bufzone.waiting == 0);
	dbg_assert (aiobuf->bufzone.enqueued == 0);

	aiobuf->aios.aio_fildes = open (aiobuf->filename, fmode,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (aiobuf->aios.aio_fildes == -1)
		return -1;

	return 0;
}

/*
 * Close a stream or index file. Reset cursor and tail of bufzone.
 * Zero the aiocb struct.
 */
static void
s_task_save_close_aiobuf (struct s_task_save_aiobuf_t* aiobuf)
{
	assert (aiobuf != NULL);

	if (aiobuf->aios.aio_fildes == -1)
		return; /* _open failed? */

	aiobuf->bufzone.waiting = 0;
	aiobuf->bufzone.enqueued = 0;
#ifdef ENABLE_FULL_DEBUG
	memset (&aiobuf->bufzone.st, 0, sizeof(aiobuf->bufzone.st));
#endif

	ftruncate (aiobuf->aios.aio_fildes, aiobuf->size);
	close (aiobuf->aios.aio_fildes);
	memset (&aiobuf->aios, 0, sizeof(aiobuf->aios));
	aiobuf->aios.aio_sigevent.sigev_notify = SIGEV_NONE;
	aiobuf->aios.aio_fildes = -1;

	aiobuf->size = 0;

	aiobuf->bufzone.cur = aiobuf->bufzone.tail =
		aiobuf->bufzone.base;
}

/*
 * Requests the index and data files be saved in hdf5 format.
 * Returns 0 on success, -1 on error.
 */
static int
s_task_save_conv_data (struct s_task_save_data_t* sjob)
{
	assert (sjob != NULL);

	uint8_t num_dsets = TSAVE_NUM_DSETS;
	struct hdf5_dset_desc_t dsets[TSAVE_NUM_DSETS] = {0,};
	for (int s = 0; s < TSAVE_NUM_DSETS ; s++)
	{
		dsets[s].filename = sjob->aio[s].filename;
		dsets[s].dname = sjob->aio[s].dataset;
		dsets[s].length = -1;
	}
	dbg_assert (num_dsets == sizeof (dsets) / sizeof (struct hdf5_dset_desc_t));
	dbg_assert (num_dsets == sizeof (sjob->aio) / sizeof (struct s_task_save_aiobuf_t));

	char filename[PATH_MAX];
	snprintf (filename, PATH_MAX, "%s.hdf5", sjob->basefname);
	struct hdf5_conv_req_t creq = {
		.filename = filename,
		.group = sjob->measurement,
		.datasets = dsets,
		.num_dsets = num_dsets,
		.ovrwt = sjob->overwrite,
		.async = sjob->async,
	};

	return hdf5_conv (&creq);
}

/*
 * Opens the stats file and reads stats. Closes it afterwards.
 * Returns 0 on success, -1 on error.
 */
static int
s_task_save_stats_read (struct s_task_save_data_t* sjob)
{
	assert (sjob != NULL);
	assert (sjob->basefname != NULL);
	dbg_assert (sjob->fd == -1);

	sjob->fd = open (sjob->basefname, O_RDONLY);
	if (sjob->fd == -1)
		return -1;

	off_t rc = read (sjob->fd, &sjob->st, TSAVE_STAT_LEN);
	close (sjob->fd);
	sjob->fd = -1;

	if (rc != TSAVE_STAT_LEN)
		return -1;
	
	return 0;
}

/*
 * Opens the stats file and writes stats. Closes it afterwards.
 * Returns 0 on success, -1 on error.
 */
static int
s_task_save_stats_write (struct s_task_save_data_t* sjob)
{
	assert (sjob != NULL);
	assert (sjob->basefname != NULL);
	dbg_assert (sjob->fd == -1);

	sjob->fd = open (sjob->basefname, O_WRONLY | O_CREAT,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (sjob->fd == -1)
		return -1;

	off_t rc = write (sjob->fd, &sjob->st, TSAVE_STAT_LEN);
	close (sjob->fd);
	sjob->fd = -1;

	if (rc != TSAVE_STAT_LEN)
		return -1;
	
	return 0;
}

/*
 * Sends the statistics to the client and resets them.
 * Returns 0 on success, -1 on error.
 */
static int
s_task_save_stats_send (struct s_task_save_data_t* sjob,
		zsock_t* frontend, uint8_t status)
{
	assert (sjob != NULL);
	assert (sjob->basefname != NULL);
	dbg_assert (sjob->fd == -1); /* _read and _write should close it */

	int rc = zsock_send (frontend, TSAVE_REP_PIC,
			status,
			sjob->st.ticks,
			sjob->st.events,
			sjob->st.traces,
			sjob->st.hists,
			sjob->st.frames,
			sjob->st.frames_lost,
			sjob->st.frames_dropped);

	memset (&sjob->st, 0, TSAVE_STAT_LEN);
	memset (&sjob->cur_stream, 0, sizeof (sjob->cur_stream));
	memset (&sjob->cur_tick, 0, sizeof (sjob->cur_tick));

	sjob->basefname = NULL; /* points to a static string */
	sjob->recording = 0;

	return rc;
}

/*
 * Blocks untils the aio jobs for all bufzones are ready.
 */
static void
s_task_save_flush (struct s_task_save_data_t* sjob)
{
	assert (sjob != NULL);

	int jobrc;
	for (int s = 0; s < TSAVE_NUM_DSETS ; s++)
	{
		do {
			jobrc = s_task_save_queue_aiobuf (&sjob->aio[s], 1);
		} while (jobrc == EINPROGRESS);
	}
}

/*
 * Copies buf to bufzone. If previous aio_write is completed and enough bytes
 * are waiting in buffer, queues them.
 * If there is no space for another packet, will block until it's done.
 * Returns 0 on success or if nothing was queued.
 * Otherwise returns same as s_task_save_queue_aiobuf.
 */
static int
s_task_save_try_queue_aiobuf (struct s_task_save_aiobuf_t* aiobuf,
	const char* buf, uint16_t len)
{
	dbg_assert (aiobuf != NULL);
	dbg_assert (aiobuf->aios.aio_fildes != -1);
	dbg_assert (buf != NULL);
	dbg_assert (len > 0);

	dbg_assert (aiobuf->bufzone.enqueued + aiobuf->bufzone.waiting <=
		TSAVE_BUFSIZE - MAX_TES_FRAME_LEN);
	dbg_assert (aiobuf->bufzone.cur >= aiobuf->bufzone.base);
	dbg_assert (aiobuf->bufzone.tail >= aiobuf->bufzone.base);
	dbg_assert (aiobuf->bufzone.cur < aiobuf->bufzone.ceil);
	dbg_assert (aiobuf->bufzone.tail + aiobuf->bufzone.enqueued <=
		aiobuf->bufzone.ceil);
	dbg_assert (aiobuf->bufzone.cur < aiobuf->bufzone.tail ||
		aiobuf->bufzone.cur >=
			aiobuf->bufzone.tail + aiobuf->bufzone.enqueued);
	dbg_assert (aiobuf->bufzone.cur == aiobuf->bufzone.tail
		+ aiobuf->bufzone.enqueued + aiobuf->bufzone.waiting -
		((aiobuf->bufzone.cur < aiobuf->bufzone.tail) ?
			 TSAVE_BUFSIZE : 0));

	/* Wrap cursor if needed */
	int reserve = len - (aiobuf->bufzone.ceil - aiobuf->bufzone.cur);
	if (likely (reserve < 0))
	{
		memcpy (aiobuf->bufzone.cur, buf, len); 
		aiobuf->bufzone.cur += len;
	}
	else
	{
		memcpy (aiobuf->bufzone.cur, buf, len - reserve); 
		if (reserve > 0)
			memcpy (aiobuf->bufzone.base,
					buf + len - reserve, reserve); 
		aiobuf->bufzone.cur = aiobuf->bufzone.base + reserve;
	}
	aiobuf->bufzone.waiting += len;

#if 1 /* 0 to skip writing */
	/* If there is < MINSIZE waiting and the cursor hasn't wrapped and
	 * there is stil space for more packets, wait. */
	if (aiobuf->bufzone.waiting < TSAVE_MINSIZE && reserve < 0 &&
		aiobuf->bufzone.enqueued + aiobuf->bufzone.waiting <=
			TSAVE_BUFSIZE - MAX_TES_FRAME_LEN)
		return 0;

	/* Try to queue next batch but don't force */
	int jobrc = s_task_save_queue_aiobuf (aiobuf, 0);
#ifdef ENABLE_FULL_DEBUG
	if (jobrc == EINPROGRESS)
		aiobuf->bufzone.st.num_skipped++;
#endif

	/* If there is no space for a full frame, force write until there is.
	 * If we are finalizingm wait for all bytes to be written. */
#ifdef ENABLE_FULL_DEBUG
	bool blocked = 0;
#endif
	while ( aiobuf->bufzone.enqueued + aiobuf->bufzone.waiting >
			TSAVE_BUFSIZE - MAX_TES_FRAME_LEN &&
		jobrc == EINPROGRESS )
	{
#ifdef ENABLE_FULL_DEBUG
		blocked = 1;
#endif
		jobrc = s_task_save_queue_aiobuf (aiobuf, 1);
	}
#ifdef ENABLE_FULL_DEBUG
	if (blocked)
		aiobuf->bufzone.st.num_blocked++;
#endif
	if (jobrc == -1)
	{
		/* TO DO: how to handle errors */
		logmsg (errno, LOG_ERR, "Could not write to file");
	}
	else if (jobrc == -2)
	{
		/* TO DO: how to handle errors */
#ifdef ENABLE_FULL_DEBUG
		logmsg (0, LOG_ERR,
			"Queued %lu bytes, wrote %lu",
			aiobuf->bufzone.enqueued,
			aiobuf->bufzone.st.last_written);
#else /* ENABLE_FULL_DEBUG */
		logmsg (0, LOG_ERR,
			"Wrote unexpected number of bytes");
#endif /* ENABLE_FULL_DEBUG */
	}

#else /* skip writing */
	int jobrc = 0;
	aiobuf->size += aiobuf->bufzone.waiting;
	aiobuf->bufzone.waiting = 0;
	aiobuf->bufzone.tail = aiobuf->bufzone.cur;
#endif /* skip writing */

	dbg_assert (aiobuf->bufzone.enqueued + aiobuf->bufzone.waiting <=
		TSAVE_BUFSIZE - MAX_TES_FRAME_LEN);
	return jobrc;
}

/*
 * Queue the next batch for aio_write-ing.
 * If force is true, will suspend if file is not ready for writing.
 * Always calls aio_return for previous job. Calls aio_return if waiting for
 * new job.
 * 
 * Returns 0 if no new bytes in the bufzone (should only happen if flushing or
 * waiting for a large batch and no space in the bufzone).
 * Returns EINPROGRESS on successful queue, or if force is false and file is
 * not ready.
 * Returns -1 on error.
 * Returns -2 if number of bytes written as reported by aio_return is
 * unexpected.
 */
static int
s_task_save_queue_aiobuf (struct s_task_save_aiobuf_t* aiobuf, bool force)
{
	dbg_assert (aiobuf != NULL);

	/* If there was no previous job, no need to do checks. */
	if (aiobuf->bufzone.enqueued == 0)
		goto prepare_next;

	/* ----------------------------------------------------------------- */
	/* Check if ready. */
	int rc = aio_error (&aiobuf->aios);
	if ( ! force && rc == EINPROGRESS )
		return EINPROGRESS;

	/* Suspend while ready. */
	if ( rc == EINPROGRESS )
	{
		const struct aiocb* aiol = &aiobuf->aios;
		rc = aio_suspend (&aiol, 1, NULL);
		if (rc == -1)
			return -1;
		rc = aio_error (&aiobuf->aios);
	}

	if (rc != 0)
	{
		dbg_assert (rc != ECANCELED && rc != EINPROGRESS);
		errno = rc; /* aio_error does not set it */
		return -1;
	}

	/* Check completion status. */
	ssize_t wrc = aio_return (&aiobuf->aios);
	if (wrc == -1 && errno == EAGAIN)
	{
#ifdef ENABLE_FULL_DEBUG
		aiobuf->bufzone.st.failed_batches++;
#endif
		goto queue_as_is; /* requeue previous batch */
	}

	if (wrc == -1)
		return -1; /* an error other than EAGAIN */
	if ((size_t)wrc != aiobuf->bufzone.enqueued)
	{
		dbg_assert (aiobuf->bufzone.enqueued > 0);
#ifdef ENABLE_FULL_DEBUG
		aiobuf->bufzone.st.last_written = wrc;
#endif
		return -2;
	}

	/* ----------------------------------------------------------------- */
prepare_next:
#ifdef ENABLE_FULL_DEBUG
	{
		int bin = aiobuf->bufzone.enqueued * (TSAVE_HISTBINS - 1) / TSAVE_BUFSIZE;
		dbg_assert (bin >= 0 && bin < TSAVE_HISTBINS);
		aiobuf->bufzone.st.batches[bin]++;
	}
	aiobuf->bufzone.st.prev_waiting = aiobuf->bufzone.waiting;
	aiobuf->bufzone.st.prev_enqueued = aiobuf->bufzone.enqueued;
#endif

	/* Increase file size by number of bytes written. */
	aiobuf->size += aiobuf->bufzone.enqueued;

	/* Release written bytes by moving the tail. */
	aiobuf->bufzone.tail += aiobuf->bufzone.enqueued;
	/* if cursor had wrapped around last time */
	if (aiobuf->bufzone.tail == aiobuf->bufzone.ceil)
		aiobuf->bufzone.tail = aiobuf->bufzone.base;
	dbg_assert (aiobuf->bufzone.tail < aiobuf->bufzone.ceil);

	/* If cursor had wrapped around, queue until the end of the
	 * bufzone. When done, tail will move to ceil, we handle
	 * this above. */
	if (unlikely (aiobuf->bufzone.cur < aiobuf->bufzone.tail))
		aiobuf->bufzone.enqueued = aiobuf->bufzone.ceil
			- aiobuf->bufzone.tail;
	else
		aiobuf->bufzone.enqueued = aiobuf->bufzone.cur
			- aiobuf->bufzone.tail;

	dbg_assert (aiobuf->bufzone.waiting >= aiobuf->bufzone.enqueued);
	aiobuf->bufzone.waiting -= aiobuf->bufzone.enqueued;

	dbg_assert (aiobuf->bufzone.waiting == 0 || aiobuf->bufzone.tail
			+ aiobuf->bufzone.enqueued == aiobuf->bufzone.ceil);

	/* ----------------------------------------------------------------- */
queue_as_is:
	dbg_assert (aiobuf->bufzone.tail != aiobuf->bufzone.ceil);
	/* Check if called in vain, should only happen at the end when flushing or
	 * if we had queued a batch larger than TSAVE_BUFSIZE - MAX_TES_FRAME_LEN. */
	if (aiobuf->bufzone.enqueued == 0)
	{
		dbg_assert (aiobuf->bufzone.waiting == 0);
		return 0;
	}

	aiobuf->aios.aio_offset = aiobuf->size;
	aiobuf->aios.aio_buf = aiobuf->bufzone.tail;
	aiobuf->aios.aio_nbytes = aiobuf->bufzone.enqueued;
	do
	{
		rc = aio_write (&aiobuf->aios);
	} while (rc == -1 && errno == EAGAIN);
	if (rc == -1)
		return -1; /* an error other than EAGAIN */
	return EINPROGRESS;
}

/*
 * Prepends TSAVE_ROOT to filename and canonicalizes the path via realpath.
 * If checkonly is false, creates any missing parent directories.
 * On success returns a pointer to a statically allocated string, caller
 * must not free it.
 * Returns NULL on error (including if checkonly is true and the filename does
 * not exist).
 * If NULL is returned because the filename is not allowed by us (i.e. outside
 * of TSAVE_ROOT or ends with a slash) errno should be 0.
 */
static char*
s_task_save_canonicalize_path (const char* filename, bool checkonly)
{
	assert (filename != NULL);

	errno = 0;
	size_t len = strlen (filename);
	if (len == 0)
	{
		logmsg (0, LOG_DEBUG, "Filename is empty");
		return NULL;
	}

#ifdef TSAVE_REQUIRE_FILENAME
	if (filename[len - 1] == '/')
	{
		logmsg (0, LOG_DEBUG,
			"Filename ends with /");
		return NULL;
	}
#endif

	/* Only one thread should use this, so static storage is fine. */
	static char finalpath[PATH_MAX];

	char buf[PATH_MAX];
	memset (buf, 0, PATH_MAX);
	snprintf (buf, PATH_MAX, "%s%s", TSAVE_ROOT, filename);

	/* Check if the file exists first. */
	errno = 0;
	char* rs = realpath (buf, finalpath);
	if (rs)
	{
		errno = 0;
		assert (rs == finalpath);
		if ( memcmp (finalpath, TSAVE_ROOT, strlen (TSAVE_ROOT)) != 0)
		{
			logmsg (0, LOG_DEBUG,
				"Resolved to %s, outside of root",
				finalpath);
			return NULL; /* outside of root */
		}
		return finalpath;
	}
	if (checkonly)
	{
		logmsg (0, LOG_DEBUG,
			"File doesn't exist");
		return NULL;
	}

	/*
	 * We proceed only if some of the directories are missing, i.e. errno
	 * is ENOENT.
	 * errno is ENOTDIR only when a component of the parent path exists but
	 * is not a directory. If filename ends with a / the part before the
	 * last slash is also considered a directory, so will return ENOTDIR if
	 * it is an existing file, but ENOENT if it doesn't exist.
	 */
	if (errno != ENOENT)
		return NULL;

	/* Start from the top-most component (after TSAVE_ROOT) and create
	 * directories as needed. */
	memset (buf, 0, PATH_MAX);
	strcpy (buf, TSAVE_ROOT);

	const char* cur_seg = filename;
	const char* next_seg = NULL;
	len = strlen (buf);
	while ( (next_seg = strchr (cur_seg, '/')) != NULL)
	{
		if (cur_seg[0] == '/')
		{ /* multiple consecutive slashes */
			cur_seg++;
			continue;
		}

		/* copy leading slash of next_seg at the end */
		assert (len < PATH_MAX);
		if (len + next_seg - cur_seg + 1 >= PATH_MAX)
		{
			logmsg (0, LOG_DEBUG,
				"Filename too long");
			return NULL;
		}
		strncpy (buf + len, cur_seg, next_seg - cur_seg + 1);
		len += next_seg - cur_seg + 1;
		assert (len == strlen (buf));

		errno = 0;
		int rc = mkdir (buf, 0777);
		if (rc && errno != EEXIST)
			return NULL; /* don't handle other errors */

		cur_seg = next_seg + 1; /* skip over leading slash */
	}

	/* Canonicalize the directory part */
	rs = realpath (buf, finalpath);
	assert (rs != NULL); /* this shouldn't happen */
	assert (rs == finalpath);

	/* Add the base filename (realpath removes the trailing slash) */
#ifdef TSAVE_REQUIRE_FILENAME
	assert (strlen (cur_seg) > 0);
#else
	/* TO DO: generate a filename is none is given */
	assert (0);
#endif
	len = strlen (finalpath);
	if (strlen (cur_seg) + len >= PATH_MAX)
	{
		logmsg (0, LOG_DEBUG,
				"Filename too long");
		return NULL;
	}

	snprintf (finalpath + len, PATH_MAX - len, "/%s", cur_seg);
	errno = 0;
	if ( memcmp (finalpath, TSAVE_ROOT, strlen (TSAVE_ROOT)) != 0)
	{
		logmsg (0, LOG_DEBUG,
				"Resolved to %s, outside of root",
				finalpath);
		return NULL; /* outside of root */
	}

	return finalpath;
}

#ifdef ENABLE_FULL_DEBUG
static void
s_task_save_dbg_stats (struct s_task_save_data_t* sjob)
{
	for (int s = 0; s < TSAVE_NUM_DSETS ; s++)
	{
		struct s_task_save_aiobuf_t* aiobuf = &sjob->aio[s];
		logmsg (0, LOG_DEBUG,
			"Dataset %s: ", aiobuf->dataset); 
		uint64_t batches_tot = 0,
			 steps = TSAVE_BUFSIZE / (TSAVE_HISTBINS - 1);
		for (int b = 0 ; b < TSAVE_HISTBINS ; b++)
		{
			logmsg (0, LOG_DEBUG,
				"     %lu B to %lu B: %lu batches",
				b*steps,
				(b+1)*steps,
				aiobuf->bufzone.st.batches[b]);
			batches_tot += aiobuf->bufzone.st.batches[b];
		}

		logmsg (0, LOG_DEBUG,
			"     Wrote %lu batches (%lu repeated, "
			"%lu skipped, %lu blocked)",
			batches_tot,
			aiobuf->bufzone.st.failed_batches,
			aiobuf->bufzone.st.num_skipped,
			aiobuf->bufzone.st.num_blocked);
	}
}
#endif
