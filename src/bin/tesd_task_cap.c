/*
 * TO DO:
 *  - Check filename for non-printable and non-ASCII characters.
 *  - Return a string error in case of a failed request or job?
 *  - FIX: why does the task count more missed packets than
 *    coordinator?
 *  - Log REQ jobs in a global database such that it can be looked
 *    up by filename, client IP or time frame.
 *  - Save the statistics as attributes in the hdf5 file.
 *  - Generate a filename is none is given.
 */

#include "tesd_tasks.h"
#include <aio.h>
#include "hdf5conv.h"

#define FIDX_LEN 16 // frame index
#define TIDX_LEN  8 // tick index
#define SIDX_LEN 16 // MCA and trace indices
#define STAT_LEN 64 // job statistics
#ifndef DATAROOT
#define DATAROOT "/media/data/captures/"
#endif

#define REQUIRE_FILENAME // for now we don't generate filename
// #define SINGLE_FILE      // save all payloads to a single .dat file
// #define SAVE_HEADERS     // save headers in .*dat files
// #define NO_BAD_FRAMES    // drop bad frames

/* Employ a buffer zone for asynchronous writing. We memcpy frames
 * into the bufzone, between its head and cursor (see s_data_t below)
 * and queue batches with aio_write.
 * aio_write has significant overhead and is not worth queueing less
 * than ~2kB (it'd be much slower than synchronous write). */
#define BUFSIZE 10485760UL // 10 MB
#define MINSIZE 512000UL   // 500 kB
#if DEBUG_LEVEL >= VERBOSE
#  define STAT_NBINS 11
#endif

/*
 * Transformed packet type byte: for the frame index
 */
struct s_ftype_t
{
	/* PT: */
#define FTYPE_PEAK       0
#define FTYPE_AREA       1
#define FTYPE_PULSE      2
#define FTYPE_TRACE_SGL  3
#define FTYPE_TRACE_AVG  4
#define FTYPE_TRACE_DP   5
#define FTYPE_TRACE_DPTR 6
#define FTYPE_TICK       7
#define FTYPE_MCA        8
#define FTYPE_BAD        9
	uint8_t PT  : 4;
	uint8_t     : 2; /* reserved */
	uint8_t HDR : 1; /* header frame in multi-frame stream */
	uint8_t SEQ : 1; /* sequence error in event stream */
};
#define linear_etype(pkt_type,tr_type) \
	( (pkt_type == TESPKT_TYPE_TRACE) ? 3 + tr_type : pkt_type )

/*
 * Statistics sent as a reply and saved to the file. 
 */
struct s_stats_t
{
	uint64_t ticks;
	uint64_t events;         // number of events written 
	uint64_t traces;         // number of traces written 
	uint64_t hists;          // number of histograms written 
	uint64_t frames;         // total frames saved
	uint64_t frames_lost;    // total frames lost
	uint64_t frames_dropped; // total frames dropped
	uint64_t errors;         // TO DO: last 8-bytes of tick header 
};

/*
 * A list of stream and index files.
 */
#ifdef SINGLE_FILE
#  define NUM_DSETS 5
#else
#  define NUM_DSETS 8
#endif
static struct s_dset_t
{
	char* dataset;   // name of dataset inside hdf5 file
	char* extension; // file extension
} s_dsets[] = {
#  define DSET_FIDX 0
	{ // frame index
		.dataset = "fidx",
		.extension = "fidx",
	},
#  define DSET_MIDX 1
	{ // MCA index
		.dataset = "midx",
		.extension = "midx",
	},
#  define DSET_TIDX 2
	{ // tick index
		.dataset = "tidx",
		.extension = "tidx",
	},
#  define DSET_RIDX 3
	{ // trace index
		.dataset = "ridx",
		.extension = "ridx",
	},
#ifdef SINGLE_FILE
#  define DSET_ADAT 4
	{ // all payloads
		.dataset = "all data",
		.extension = "adat",
	},
#else
#  define DSET_BDAT 4
	{ // bad payloads
		.dataset = "bad",
		.extension = "bdat",
	},
#  define DSET_MDAT 5
	{ // MCA payloads
		.dataset = "mca",
		.extension = "mdat",
	},
#  define DSET_TDAT 6
	{ // tick payloads
		.dataset = "ticks",
		.extension = "tdat",
	},
#  define DSET_EDAT 7
	{ // event payloads
		.dataset = "events",
		.extension = "edat",
	},
#endif
};

/*
 * Data related to a stream or index file, e.g. ticks or MCA frames.
 */
struct s_aiobuf_t
{
	struct aiocb aios;
	struct
	{
		unsigned char* base; // mmapped, size of BUFSIZE
		unsigned char* tail; // start address queued for aio_write
		unsigned char* cur;  // address of next packet
		unsigned char* ceil; // base + BUFSIZE
		size_t waiting;      // copied to buffer since last aio_write
		size_t enqueued;     // queued for writing at last aio_write
#if DEBUG_LEVEL >= VERBOSE
		struct
		{
			size_t prev_enqueued;
			size_t prev_waiting;
			size_t last_written;
			uint64_t batches[STAT_NBINS];
			uint64_t failed_batches;
			uint64_t num_skipped;
			uint64_t num_blocked;
		} st;
#endif
	} bufzone;
	size_t size; // number of bytes written
	char   filename[PATH_MAX]; // name data/index file
	char*  dataset;            // name of dataset inside hdf5 file
	                           // points to one of the literal
	                           // strings in s_dsets
};

/*
 * The frame index.
 * Flags mca, bad and seq are set in event type.
 */
struct s_fidx_t
{
	uint64_t start;   // frame's offset into its dat file
	uint32_t length;  // payload's length
	uint16_t esize;   // original event size
	uint8_t  changed; // event frame differs from previous
	struct s_ftype_t ftype; // see definition of struct
};

/*
 * The tick index.
 */
struct s_tidx_t
{
	uint32_t start_frame; // frame number of first non-tick event
	uint32_t stop_frame;  // frame number of last non-tick event
};

/*
 * The MCA and trace indices. (the 's' is for 'stream')
 */
struct s_sidx_t
{
	uint64_t start;  // first byte of histogram/trace into dat file
	uint64_t length; // length in bytes of histogram/trace
};

/*
 * Data for the currently-saved file. min_ticks and basefname are
 * set when receiving a request from client.
 */
struct s_data_t
{
	struct s_stats_t st;
	struct s_aiobuf_t aio[NUM_DSETS];

	struct
	{ /* keep track of multi-frame streams */
		struct s_sidx_t idx;
		size_t size;
		size_t cur_size;
		bool is_event; // i.e. is_trace, otherwise it's MCA
		bool discard;  // stream had errors, ignore rest
	} cur_stream;

	struct
	{
		struct s_tidx_t idx;
		uint32_t nframes;  // no. of event frames in this tick
	} cur_tick;
	uint8_t  prev_esize; // event size for previous event
	uint8_t  prev_etype; // event type for previous event,
	                     // see s_ftype_t

	struct
	{ /* given by client */
		uint64_t min_ticks;   // cpature at least that many ticks
		uint64_t min_events;  // cpature at least that many events
		uint8_t  ovrwtmode;   // TES_H5_OVRT_*, see hdf5conv.h
		uint8_t  async;       // copy data to hdf5 in the background
		uint8_t  capmode;     // only convert a previous capture
		char*    basefname;   // datafiles will be
		                      // <basefname>-<measurement>.*
		char*    measurement; // hdf5 group
	};
	/* next three are set for convenience */
	bool     nocapture;   // request is for status or conversion
	bool     noconvert;   // request is for status or capture only
	bool     nooverwrite; // overwrite data files

	char     hdf5filename[PATH_MAX]; // full path of hdf5 file
	char     statfilename[PATH_MAX]; // full path of stats file
	int      statfd;      // fd for the statis file
	bool     recording;   // wait for a tick before starting capture
};

/* Task initializer and finalizer. */
static int   s_init_aiobuf (struct s_aiobuf_t* aiobuf);
static void  s_fin_aiobuf (struct s_aiobuf_t* aiobuf);

/*
 * s_open and s_close deal with stream and index files only. stats_*
 * deal with the stats file/database. s_stats_send should be called at
 * the very end (after processing is done).
 */
/* Job initializer and finalizer. */
static int  s_is_req_valid (struct s_data_t* sjob);
static int  s_task_construct_filenames (struct s_data_t* sjob);
static int  s_open (struct s_data_t* sjob, mode_t fmode);
static void s_close (struct s_data_t* sjob);
static int  s_open_aiobuf (struct s_aiobuf_t* aiobuf, mode_t fmode);
static void s_close_aiobuf (struct s_aiobuf_t* aiobuf);
static int  s_conv_data (struct s_data_t* sjob);
static void s_send_err (struct s_data_t* sjob,
	zsock_t* frontend, uint8_t status);

/* Statistics for a job. */
static int s_stats_read (struct s_data_t* sjob);
static int s_stats_write (struct s_data_t* sjob);
static int s_stats_send (struct s_data_t* sjob,
	zsock_t* frontend, uint8_t status);

/* Ongoing job helpers. */
static void  s_flush (struct s_data_t* sjob);
static int   s_try_queue_aiobuf (struct s_aiobuf_t* aiobuf,
	const char* buf, uint16_t len);
static int   s_queue_aiobuf (struct s_aiobuf_t* aiobuf, bool force);
static char* s_canonicalize_path (const char* filename,
	char* finalpath, bool mustexist);

#if DEBUG_LEVEL >= VERBOSE
static void  s_dbg_stats (struct s_data_t* sjob);
#endif

/* -------------------------------------------------------------- */
/* --------------------------- HELPERS -------------------------- */
/* -------------------------------------------------------------- */

/*
 * mmap data for a stream or index file.
 * Returns 0 on success, -1 on error.
 */
static int
s_init_aiobuf (struct s_aiobuf_t* aiobuf)
{
	assert (aiobuf != NULL);

	aiobuf->aios.aio_sigevent.sigev_notify = SIGEV_NONE;
	aiobuf->aios.aio_fildes = -1;

	void* buf = mmap (NULL, BUFSIZE, PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == (void*)-1)
		return -1;

	aiobuf->bufzone.base = aiobuf->bufzone.tail =
		aiobuf->bufzone.cur = (unsigned char*) buf;
	aiobuf->bufzone.ceil = aiobuf->bufzone.base + BUFSIZE;

	return 0;
}

/*
 * munmap data for a stream or index file.
 */
static void
s_fin_aiobuf (struct s_aiobuf_t* aiobuf)
{
	assert (aiobuf != NULL);

	/* Unmap bufzone */
	if (aiobuf->bufzone.base != NULL)
	{
		munmap (aiobuf->bufzone.base, BUFSIZE);
		aiobuf->bufzone.base = NULL;
	}
}

/*
 * Check if request is valid, set useful internal flags.
 * Returns TES_CAP_REQ_*
 */
static int
s_is_req_valid (struct s_data_t* sjob)
{
	if (sjob->basefname == NULL)
	{
		logmsg (0, LOG_ERR, "Invalid request");
		return TES_CAP_REQ_EINV;
	}

	if (sjob->measurement == NULL)
	{ /* The client did not send a frame for the measurement, default to
		 * empty string. We free this later so it must be malloc'ed rather
		 * than static. */
		sjob->measurement = (char*) malloc (1);
		if (sjob->measurement == NULL)
		{
			logmsg ((errno == ENOMEM) ? 0 : errno, LOG_ERR,
				"Cannot allocate memory");
			return TES_CAP_REQ_EFAIL;
		}
		sjob->measurement[0] = '\0';
	}

	switch (sjob->ovrwtmode)
	{
		case TES_H5_OVRWT_NONE:
		case TES_H5_OVRWT_RELINK:
		case TES_H5_OVRWT_FILE:
			break;
		default:
			logmsg (0, LOG_ERR, "Invalid overwrite mode");
			return TES_CAP_REQ_EINV;
	}

	switch (sjob->capmode)
	{
		case TES_CAP_AUTO:
		case TES_CAP_CAPONLY:
		case TES_CAP_CONVONLY:
			break;
		default:
			logmsg (0, LOG_ERR, "Invalid capture mode");
			return TES_CAP_REQ_EINV;
	}

	/* Does it require capture? */
	/* if min events was given, min ticks default to 1 */
	if (sjob->min_events != 0 && sjob->min_ticks == 0)
		sjob->min_ticks = 1;
	sjob->nocapture = (sjob->min_ticks == 0);

	if ( (sjob->capmode == TES_CAP_CONVONLY && ! sjob->nocapture) ||
		(sjob->capmode == TES_CAP_CAPONLY && sjob->nocapture) )
	{
			logmsg (0, LOG_ERR, "Ambiguous request");
			return TES_CAP_REQ_EINV;
	}

	bool statusonly = (sjob->nocapture &&
		sjob->capmode != TES_CAP_CONVONLY);

	/* Does it require conversion? */
	sjob->noconvert = (statusonly || sjob->capmode == TES_CAP_CAPONLY);

	/* Should we overwrite data files. */
	sjob->nooverwrite = (sjob->ovrwtmode == TES_H5_OVRWT_NONE);

	return TES_CAP_REQ_OK;
}

/*
 * Set the stats, hdf5, index and data filenames as well as the dataset
 * names.
 * Returns TES_CAP_REQ_*
 */
static int
s_task_construct_filenames (struct s_data_t* sjob)
{
	assert (sjob != NULL);
	assert (sjob->basefname != NULL);
	assert (sjob->measurement != NULL);

	char tmpfname[PATH_MAX];

	/* Statistics file. */
	int rc = snprintf (tmpfname, PATH_MAX, "%s%s%s",
		sjob->basefname,
		strlen (sjob->measurement) == 0 ? "" : "/",
		sjob->measurement);
	if (rc == -1 || (size_t)rc >= PATH_MAX)
	{
		logmsg (rc == -1 ? errno : 0, LOG_ERR,
			"Cannot construct filename for dataset %s%s%s",
			sjob->basefname,
			strlen (sjob->measurement) == 0 ? "" : "/",
			sjob->measurement);
		return TES_CAP_REQ_EFAIL;
	}
	char* tmpfname_p = s_canonicalize_path (
		tmpfname, sjob->statfilename, sjob->nocapture);
	if (tmpfname_p == NULL)
	{
		if (sjob->nocapture)
		{
			logmsg (0, LOG_INFO, "Job not found");
			return TES_CAP_REQ_EABORT;
		}
		else
		{
			logmsg (errno, LOG_INFO, "Filename is not valid");
			return TES_CAP_REQ_EPERM;
		}
	}

	/* HDF5 file. */
	tmpfname_p = NULL;
	if (strchr (sjob->measurement, '/') == NULL)
	{ /* make sure measurement group does not contain a slash. */
		snprintf (tmpfname, PATH_MAX, "%s.hdf5", sjob->basefname);
		tmpfname_p = s_canonicalize_path (
			tmpfname, sjob->hdf5filename, false);
	}
	if (tmpfname_p == NULL)
	{ /* it had a slash or it was a symlink to outside DATAROOT */
		logmsg (errno, LOG_INFO, "HDF5 filename is not valid");
		return TES_CAP_REQ_EPERM;
	}

	/* Index and data files. */
	for (int s = 0; s < NUM_DSETS ; s++)
	{
		struct s_aiobuf_t* aiobuf = &sjob->aio[s];
		aiobuf->dataset = s_dsets[s].dataset;

		rc = snprintf (aiobuf->filename, PATH_MAX, "%s.%s",
			sjob->statfilename,
			s_dsets[s].extension);
		if (rc == -1 || (size_t)rc >= PATH_MAX)
		{
			logmsg (rc == -1 ? errno : 0, LOG_ERR,
				"Cannot construct filename for dataset");
			return TES_CAP_REQ_EFAIL;
		}
	}
  
	return TES_CAP_REQ_OK;
}

/*
 * Opens the stream and index files.
 * It does not close any successfully opened files are closed if an
 * error occurs.
 * Returns TES_CAP_REQ_*
 */
static int
s_open (struct s_data_t* sjob, mode_t fmode)
{
	assert (sjob != NULL);

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
	for (int s = 0; s < NUM_DSETS ; s++)
	{
		struct s_aiobuf_t* aiobuf = &sjob->aio[s];
		int rc = s_open_aiobuf (aiobuf, fmode);
		if (rc == -1)
		{
			if (sjob->nooverwrite)
			{
				logmsg (0, LOG_INFO, "Not going to overwrite");
				return TES_CAP_REQ_EABORT;
			}
			else
			{
				logmsg (errno, LOG_ERR, "Could not open files '%s.*'",
					sjob->statfilename);
				return TES_CAP_REQ_EFAIL;
			}
		}
	}

	return TES_CAP_REQ_OK;
}

/*
 * Closes the stream and index files.
 */
static void
s_close (struct s_data_t* sjob)
{
	assert (sjob != NULL);

	/* Close the data files. */
	for (int s = 0; s < NUM_DSETS ; s++)
		s_close_aiobuf (&sjob->aio[s]);
}

/*
 * Open a stream or index file.
 * Returns 0 on success, -1 on error.
 */
static int
s_open_aiobuf (struct s_aiobuf_t* aiobuf, mode_t fmode)
{
	assert (aiobuf != NULL);
	assert (aiobuf->filename != NULL);

	dbg_assert (aiobuf->aios.aio_fildes == -1);
	dbg_assert (aiobuf->size == 0);
	dbg_assert (aiobuf->bufzone.cur == aiobuf->bufzone.tail);
	dbg_assert (aiobuf->bufzone.cur == aiobuf->bufzone.base);
	dbg_assert (aiobuf->bufzone.waiting == 0);
	dbg_assert (aiobuf->bufzone.enqueued == 0);

	/* If overwriting, unlink the file first to prevent permission
	 * errors if owned by another user and to avoid writing outside of
	 * root if data file is a symlink (it is not checked with
	 * s_canonicalize_path). */
	if (! (fmode & O_EXCL))
	{
		int fok = access (aiobuf->filename, F_OK);
		if (fok == 0)
		{
			int rc = unlink (aiobuf->filename);
			if (rc == -1)
				return -1;
		}
	}

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
s_close_aiobuf (struct s_aiobuf_t* aiobuf)
{
	assert (aiobuf != NULL);

	if (aiobuf->aios.aio_fildes == -1)
		return; /* _open failed? */

	aiobuf->bufzone.waiting = 0;
	aiobuf->bufzone.enqueued = 0;
#if DEBUG_LEVEL >= VERBOSE
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
 * Returns TES_CAP_REQ_*
 */
static int
s_conv_data (struct s_data_t* sjob)
{
	assert (sjob != NULL);

	struct hdf5_dset_desc_t dsets[NUM_DSETS] = {0};
	for (int s = 0; s < NUM_DSETS ; s++)
	{
		dsets[s].filename = sjob->aio[s].filename;
		dsets[s].dsetname = sjob->aio[s].dataset;
		dsets[s].length = -1;
	}

	struct hdf5_conv_req_t creq = {
		.filename = sjob->hdf5filename,
		.group = sjob->measurement,
		.dsets = dsets,
		.num_dsets = NUM_DSETS,
		.ovrwtmode = sjob->ovrwtmode,
		.async = (sjob->async != 0),
	};

	int rc = hdf5_conv (&creq);
	if (rc != TES_CAP_REQ_OK)
		logmsg (errno, LOG_ERR, "Could not convert data to hdf5");

	return rc;
}

/*
 * Sends an error to client.
 */
static void
s_send_err (struct s_data_t* sjob,
	zsock_t* frontend, uint8_t status)
{
	zsock_send (frontend, TES_CAP_REP_PIC, status, 0, 0, 0, 0, 0, 0, 0);

	zstr_free (&sjob->basefname);   /* nullifies the pointer */
	zstr_free (&sjob->measurement); /* nullifies the pointer */
}

/*
 * Opens the stats file and reads stats. Closes it afterwards.
 * Returns TES_CAP_REQ_*
 */
static int
s_stats_read (struct s_data_t* sjob)
{
	assert (sjob != NULL);
	assert (sjob->basefname != NULL);
	dbg_assert (sjob->statfd == -1);

	sjob->statfd = open (sjob->statfilename, O_RDONLY);
	if (sjob->statfd == -1)
	{
		logmsg (errno, LOG_ERR, "Could not open stats file");
		/* The stat file is ensured to be present at this point. */
		return TES_CAP_REQ_EFAIL;
	}

	off_t rc = read (sjob->statfd, &sjob->st, STAT_LEN);
	close (sjob->statfd);
	sjob->statfd = -1;

	if (rc != STAT_LEN)
	{
		logmsg (errno, LOG_ERR, "Could not read stats");
		return TES_CAP_REQ_EFAIL;
	}
	
	return TES_CAP_REQ_OK;
}

/*
 * Opens the stats file and writes stats. Closes it afterwards.
 * Returns TES_CAP_REQ_*
 */
static int
s_stats_write (struct s_data_t* sjob)
{
	assert (sjob != NULL);
	assert (sjob->basefname != NULL);
	dbg_assert (sjob->statfd == -1);

	/* s_req_hn should unlink it, ensure this with O_EXCL */
	sjob->statfd = open (sjob->statfilename,
		O_WRONLY | O_CREAT | O_EXCL,
		S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (sjob->statfd == -1)
	{
		logmsg (errno, LOG_ERR, "Could not open stats file");
		return TES_CAP_REQ_EFIN;
	}

	off_t rc = write (sjob->statfd, &sjob->st, STAT_LEN);
	close (sjob->statfd);
	sjob->statfd = -1;

	if (rc != STAT_LEN)
	{
		logmsg (errno, LOG_ERR, "Could not write stats");
		return TES_CAP_REQ_EFIN;
	}
	
	return TES_CAP_REQ_OK;
}

/*
 * Sends the statistics to the client and resets them.
 * Returns TES_CAP_REQ_*
 */
static int
s_stats_send (struct s_data_t* sjob,
	zsock_t* frontend, uint8_t status)
{
	assert (sjob != NULL);
	assert (sjob->basefname != NULL);
	dbg_assert (sjob->statfd == -1); /* _read and _write close it */

	int rc = zsock_send (frontend, TES_CAP_REP_PIC,
		status,
		sjob->st.ticks,
		sjob->st.events,
		sjob->st.traces,
		sjob->st.hists,
		sjob->st.frames,
		sjob->st.frames_lost,
		sjob->st.frames_dropped);

	memset (&sjob->st, 0, STAT_LEN);
	memset (&sjob->cur_stream, 0, sizeof (sjob->cur_stream));
	memset (&sjob->cur_tick, 0, sizeof (sjob->cur_tick));

	zstr_free (&sjob->basefname);   /* nullifies the pointer */
	zstr_free (&sjob->measurement); /* nullifies the pointer */
	sjob->recording = false;

	return (rc ? TES_CAP_REQ_EFIN : TES_CAP_REQ_OK);
}

/*
 * Blocks untils the aio jobs for all bufzones are ready.
 */
static void
s_flush (struct s_data_t* sjob)
{
	assert (sjob != NULL);

	int jobrc;
	for (int s = 0; s < NUM_DSETS ; s++)
	{
		do
		{
			jobrc = s_queue_aiobuf (&sjob->aio[s], true);
		} while (jobrc == EINPROGRESS);
	}
}

/*
 * Copies buf to bufzone. If previous aio_write is completed and
 * enough bytes are waiting in buffer, queues them.
 * If there is no space for another packet, will block until it's
 * done.
 * Returns 0 on success or if nothing was queued.
 * Otherwise returns same as s_queue_aiobuf.
 */
static int
s_try_queue_aiobuf (struct s_aiobuf_t* aiobuf,
	const char* buf, uint16_t len)
{
	dbg_assert (aiobuf != NULL);
	dbg_assert (aiobuf->aios.aio_fildes != -1);
	dbg_assert (buf != NULL);
	dbg_assert (len > 0);

	dbg_assert (aiobuf->bufzone.enqueued + aiobuf->bufzone.waiting <=
		BUFSIZE - TESPKT_MTU);
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
				 BUFSIZE : 0));

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
			memcpy (aiobuf->bufzone.base, buf + len - reserve, reserve); 
		aiobuf->bufzone.cur = aiobuf->bufzone.base + reserve;
	}
	aiobuf->bufzone.waiting += len;

#if 1 /* 0 to skip writing */
	/* If there is < MINSIZE waiting and the cursor hasn't wrapped
	 * and there is stil space for more packets, wait. */
	if (aiobuf->bufzone.waiting < MINSIZE && reserve < 0 &&
		aiobuf->bufzone.enqueued + aiobuf->bufzone.waiting <=
			BUFSIZE - TESPKT_MTU)
		return 0;

	/* Try to queue next batch but don't force */
	int jobrc = s_queue_aiobuf (aiobuf, false);
#if DEBUG_LEVEL >= VERBOSE
	if (jobrc == EINPROGRESS)
		aiobuf->bufzone.st.num_skipped++;
#endif

	/* If there is no space for a full frame, force write until
	 * there is. If we are finalizingm wait for all bytes to be
	 * written. */
#if DEBUG_LEVEL >= VERBOSE
	bool blocked = false;
#endif
	while ( aiobuf->bufzone.enqueued + aiobuf->bufzone.waiting >
		BUFSIZE - TESPKT_MTU && jobrc == EINPROGRESS )
	{
#if DEBUG_LEVEL >= VERBOSE
		blocked = true;
#endif
		jobrc = s_queue_aiobuf (aiobuf, true);
	}
#if DEBUG_LEVEL >= VERBOSE
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
#if DEBUG_LEVEL >= VERBOSE
		logmsg (0, LOG_ERR, "Queued %lu bytes, wrote %lu",
			aiobuf->bufzone.enqueued,
			aiobuf->bufzone.st.last_written);
#else /* DEBUG_LEVEL >= VERBOSE */
		logmsg (0, LOG_ERR, "Wrote unexpected number of bytes");
#endif /* DEBUG_LEVEL >= VERBOSE */
	}

#else /* skip writing */
	int jobrc = 0;
	aiobuf->size += aiobuf->bufzone.waiting;
	aiobuf->bufzone.waiting = 0;
	aiobuf->bufzone.tail = aiobuf->bufzone.cur;
#endif /* skip writing */

	dbg_assert (aiobuf->bufzone.enqueued + aiobuf->bufzone.waiting <=
		BUFSIZE - TESPKT_MTU);
	return jobrc;
}

/*
 * Queue the next batch for aio_write-ing.
 * If force is true, will suspend if file is not ready for writing.
 * Always calls aio_return for previous job. Calls aio_return if
 * waiting for new job.
 * 
 * Returns 0 if no new bytes in the bufzone (should only happen if
 * flushing or waiting for a large batch and no space in the
 * bufzone).
 * Returns EINPROGRESS on successful queue, or if force is false and
 * file is not ready.
 * Returns -1 on error.
 * Returns -2 if number of bytes written as reported by aio_return
 * is unexpected.
 */
static int
s_queue_aiobuf (struct s_aiobuf_t* aiobuf, bool force)
{
	dbg_assert (aiobuf != NULL);

	/* If there was no previous job, no need to do checks. */
	if (aiobuf->bufzone.enqueued == 0)
		goto prepare_next;

	/* ---------------------------------------------------------- */
	/* Check if ready. */
	int rc = aio_error (&aiobuf->aios);
	if ( ! force && rc == EINPROGRESS )
		return EINPROGRESS;

	/* Suspend while ready. */
	if ( rc == EINPROGRESS )
	{
		const struct aiocb* aiol[1] = { &aiobuf->aios, };
		rc = aio_suspend (aiol, 1, NULL);
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
#if DEBUG_LEVEL >= VERBOSE
		aiobuf->bufzone.st.failed_batches++;
#endif
		goto queue_as_is; /* requeue previous batch */
	}

	if (wrc == -1)
		return -1; /* an error other than EAGAIN */
	if ((size_t)wrc != aiobuf->bufzone.enqueued)
	{
		dbg_assert (aiobuf->bufzone.enqueued > 0);
#if DEBUG_LEVEL >= VERBOSE
		aiobuf->bufzone.st.last_written = wrc;
#endif
		return -2;
	}

	/* ---------------------------------------------------------- */
prepare_next:
#if DEBUG_LEVEL >= VERBOSE
	{
		int bin = aiobuf->bufzone.enqueued *
			(STAT_NBINS - 1) / BUFSIZE;
		dbg_assert (bin >= 0 && bin < STAT_NBINS);
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

	/* ---------------------------------------------------------- */
queue_as_is:
	dbg_assert (aiobuf->bufzone.tail != aiobuf->bufzone.ceil);
	/* Check if called in vain, should only happen at the end when
	 * flushing or if we had queued a batch larger than
	 * BUFSIZE - TESPKT_MTU. */
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

static char*
s_canonicalize_path (const char* filename,
	char* finalpath, bool mustexist)
{
	assert (filename != NULL);
	assert (finalpath != NULL);

	errno = 0;
	size_t len = strlen (filename);
	if (len == 0)
	{
		logmsg (0, LOG_DEBUG, "Filename is empty");
		return NULL;
	}

#ifdef REQUIRE_FILENAME
	if (filename[len - 1] == '/')
	{
		logmsg (0, LOG_DEBUG, "Filename ends with /");
		return NULL;
	}
#endif

	return canonicalize_path (DATAROOT, filename, finalpath,
		mustexist, 0777);
}

#if DEBUG_LEVEL >= VERBOSE
static void
s_dbg_stats (struct s_data_t* sjob)
{
	for (int s = 0; s < NUM_DSETS ; s++)
	{
		struct s_aiobuf_t* aiobuf = &sjob->aio[s];
		logmsg (0, LOG_DEBUG, "Dataset %s: ", aiobuf->dataset); 
		uint64_t batches_tot = 0, steps = BUFSIZE / (STAT_NBINS - 1);
		for (int b = 0 ; b < STAT_NBINS ; b++)
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

/* -------------------------------------------------------------- */
/* ----------------------------- API ---------------------------- */
/* -------------------------------------------------------------- */

/*
 * Called when a client sends a request on the REP socket. For valid
 * requests of status, opens the file and send the reply. For valid
 * requests to save, opens the file and marks the task as active.
 */
int
task_cap_req_hn (zloop_t* loop, zsock_t* frontend, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;

	struct s_data_t* sjob = (struct s_data_t*) self->data;
	dbg_assert ( ! sjob->recording );

	int rc = zsock_recv (frontend, TES_CAP_REQ_PIC,
		&sjob->basefname,
		&sjob->measurement,
		&sjob->min_ticks,
		&sjob->min_events,
		&sjob->ovrwtmode,
		&sjob->async,
		&sjob->capmode);
	/* Would also return -1 if picture contained a pointer (p) or a null
	 * frame (z) but message received did not match this signature; this
	 * is irrelevant in this case; we don't get interrupted, this should
	 * not happen. */
	assert (rc != -1);

	/* Is the request understood? */
	rc = s_is_req_valid (sjob);
	if (rc != TES_CAP_REQ_OK)
	{
		s_send_err (sjob, frontend, rc);
		return 0;
	}

	if (sjob->nocapture)
	{
		logmsg (0, LOG_INFO,
				"Received request for %s of '%s' and measurement '%s'%s",
				sjob->noconvert ? "status" : "conversion",
				sjob->basefname,
				sjob->measurement,
				( ! sjob->noconvert && sjob->async ) ?
				". Convering asynchronously" : "");
	}
	else
	{
		logmsg (0, LOG_INFO,
				"Received request to write %lu ticks and "
				"%lu events to '%s' and measurement '%s'%s",
				sjob->min_ticks,
				sjob->min_events,
				sjob->basefname,
				sjob->measurement,
				sjob->async ? ". Convering asynchronously" : "");
	}

	/* Set the filenames and dataset names, will detect if the stats file
	 * is missing and we are not capturing, i.e. aborted job. */
	rc = s_task_construct_filenames (sjob);
	if (rc != TES_CAP_REQ_OK)
	{
		s_send_err (sjob, frontend, rc);
		return 0;
	}

	/* -------------------------------------------------- */
	/*              Status or convert query.              */
	/* -------------------------------------------------- */

	if (sjob->nocapture)
	{
		if ( ! sjob->noconvert )
		{
			rc = s_conv_data (sjob);
			if (rc != TES_CAP_REQ_OK)
			{
				s_send_err (sjob, frontend, rc);
				return 0;
			}
		}

		/* Read in stats and send reply either way. */
		rc = s_stats_read (sjob);
		if (rc != TES_CAP_REQ_OK)
		{
			s_send_err (sjob, frontend, rc);
			return 0;
		}

		rc = s_stats_send (sjob, frontend, TES_CAP_REQ_OK);
		if (rc != TES_CAP_REQ_OK)
		{
			logmsg (0, LOG_NOTICE, "Could not send stats");
		}
		return 0;
	}

	dbg_assert ( ! sjob->nocapture ); /* forgot to return? */

	/* -------------------------------------------------- */
	/*                    Write query.                    */
	/* -------------------------------------------------- */

	/* If not overwriting add O_EXCL, s_open_aiobuf will then
	 * not unlink the data files and the open call will fail. */
	mode_t fmode = O_RDWR | O_CREAT;
	if (sjob->nooverwrite)
		fmode |= O_EXCL;

	rc = s_open (sjob, fmode);
	if (rc != TES_CAP_REQ_OK)
	{
		s_send_err (sjob, frontend, rc);
		s_close (sjob);
		return 0;
	}

	logmsg (0, LOG_INFO, "Opened files '%s.*' for writing",
		sjob->statfilename);

	/* Unlink stat file to prevent permission errors later when writing */
	int fok = access (sjob->statfilename, F_OK);
	if (fok == 0)
	{
		rc = unlink (sjob->statfilename);
		if (rc == -1)
		{
			logmsg (errno, LOG_ERR, "Could not delete stat file");
			s_send_err (sjob, frontend, TES_CAP_REQ_EFAIL);

			s_close (sjob);
			return 0;
		}
	}

	/* Disable polling on the frontend until the job is done. Wakeup
	 * packet handler. */
	task_activate (self);

	return 0;
}

/*
 * Saves packet payloads to corresponding file(s) and writes index
 * files.
 */
int
task_cap_pkt_hn (zloop_t* loop, tespkt* pkt, uint16_t flen,
	uint16_t missed, int err, task_t* self)
{
	dbg_assert (self != NULL);

	struct s_data_t* sjob = (struct s_data_t*) self->data;

	bool is_tick = tespkt_is_tick (pkt);
	if ( ! sjob->recording && is_tick )
		sjob->recording = true; /* start the capture */

	if ( ! sjob->recording )
		return 0;

	if (err)
	{
#ifdef NO_BAD_FRAMES
		/* drop bad frames */
		sjob->st.frames_dropped++;
		return 0;
#endif
	}

	sjob->st.frames++;
	sjob->st.frames_lost += missed;

	uint16_t esize = tespkt_esize (pkt);
	esize = htofs (esize); /* in FPGA byte-order */
	uint16_t paylen = flen - TESPKT_HDR_LEN;
#ifdef SAVE_HEADERS
	uint16_t datlen = flen;
	char* datstart = (char*)pkt;
#else
	uint16_t datlen = paylen;
	char* datstart = (char*)pkt + TESPKT_HDR_LEN;
#endif

	bool is_header = tespkt_is_header (pkt);
	bool is_mca = tespkt_is_mca (pkt);
	bool is_trace = tespkt_is_trace_long (pkt);

	/* *************** Update tick and frame indices ***************
	 * ***************** and choose the data file. ************** */
	struct s_aiobuf_t* aiofidx = &sjob->aio[DSET_FIDX];
#ifdef SINGLE_FILE
	struct s_aiobuf_t* aiodat = &sjob->aio[DSET_ADAT];
#else
	struct s_aiobuf_t* aiodat = NULL; /* set later */
#endif
	struct s_fidx_t fidx = {0};
	fidx.length = datlen;
	fidx.esize = esize;
	fidx.changed = 0;
	fidx.ftype.SEQ = 0;

	bool finishing = 0;
	int jobrc;

	/* Check for sequence error. */
	if (missed > 0)
	{
#if DEBUG_LEVEL >= LETS_GET_NUTS
		logmsg (0, LOG_DEBUG, "Missed %hu at frame #%lu",
			missed, sjob->st.frames - 1);
#endif
		fidx.ftype.SEQ = 1;
	}

	/* Check packet type. */
	if (err)
	{
		fidx.ftype.PT = FTYPE_BAD;
#ifndef SINGLE_FILE
		aiodat = &sjob->aio[DSET_BDAT];
#endif
	}
	else if (is_mca)
	{
		fidx.ftype.PT = FTYPE_MCA;
#ifndef SINGLE_FILE
		aiodat = &sjob->aio[DSET_MDAT];
#endif
	}
	else if (is_tick)
	{
		fidx.ftype.PT = FTYPE_TICK;
#ifndef SINGLE_FILE
		aiodat = &sjob->aio[DSET_TDAT];
#endif

		if (sjob->st.ticks > 0)
		{
			struct s_tidx_t* tidx = &sjob->cur_tick.idx;
			jobrc = s_try_queue_aiobuf (
				&sjob->aio[DSET_TIDX], (char*)tidx, TIDX_LEN);
			if (jobrc < 0)
				finishing = 1; /* error */
		}

		sjob->cur_tick.nframes = 0;
		/* no need to zero the index */
	}
	else
	{
#ifndef SINGLE_FILE
		aiodat = &sjob->aio[DSET_EDAT];
#endif

		struct s_tidx_t* tidx = &sjob->cur_tick.idx;
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

	/* ********************* Update statistics *********************
	 * ********************* and stream index. *********************
	 *
	 * Check if there is an ongoing stream (trace or MCA). If so,
	 * update index if necessary. If this is the last frame of
	 * a stream, queue the index for writing and reset cur_stream's
	 * size and cur_size. size and cur_size would also be reset if
	 * an error (e.g. missed frames) occurs. idx and is_event are
	 * set when receiving the header of a new stream.
	 */

	/* Skip if frame is bad */
	if (err)
		goto done;

	if (sjob->cur_stream.size > 0)
	{
		dbg_assert (sjob->cur_stream.cur_size > 0);
		dbg_assert (sjob->cur_stream.cur_size <
			sjob->cur_stream.size);
		dbg_assert ( ! sjob->cur_stream.discard );

	}
	else
		dbg_assert (sjob->cur_stream.cur_size == 0);

	bool continues_stream = (
		(
			(is_trace && sjob->cur_stream.is_event) ||
				(is_mca && ! sjob->cur_stream.is_event)
		) && sjob->cur_stream.size > 0 &&
			 ! is_header && missed == 0 );
	bool starts_stream = ( (is_trace || is_mca) && is_header &&
		sjob->cur_stream.size == 0 );
	bool interrupts_stream = ( ! continues_stream &&
		sjob->cur_stream.size > 0 );

	if (interrupts_stream) 
	{
		sjob->cur_stream.discard = true;
		sjob->cur_stream.size = 0;
		sjob->cur_stream.cur_size = 0;

		dbg_assert ( is_header || missed > 0 ||
			(is_trace && ! sjob->cur_stream.is_event) ||
			(is_mca && sjob->cur_stream.is_event) ||
			( ! is_trace && ! is_mca ) );
#if DEBUG_LEVEL >= LETS_GET_NUTS
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
				sjob->cur_stream.is_event = true;
			}
			else
			{
				sjob->cur_stream.size = tespkt_mca_size (pkt);
				sjob->cur_stream.is_event = false;
			}
			sjob->cur_stream.discard = false;

			sjob->cur_stream.idx.start = aiodat->size +
				aiodat->bufzone.waiting + aiodat->bufzone.enqueued;

			fidx.ftype.HDR = 1;
		}
		else
		{ /* ongoing multi-frame stream */
			dbg_assert ( ! sjob->cur_stream.discard && missed == 0 );
		}

		sjob->cur_stream.cur_size += paylen;
		if (sjob->cur_stream.cur_size > sjob->cur_stream.size)
		{ /* extra bytes */
#if DEBUG_LEVEL >= LETS_GET_NUTS
			logmsg (0, LOG_DEBUG, "Extra %s data "
				"at frame #%lu",
				is_mca ? "histogram" : "trace",
				sjob->st.frames - 1);
#endif
			sjob->cur_stream.size = 0;
			sjob->cur_stream.cur_size = 0;
			sjob->cur_stream.discard = true;
		}
		else if (sjob->cur_stream.cur_size == sjob->cur_stream.size)
		{ /* done, record the event */
			struct s_aiobuf_t* aiosidx = NULL;
			if (is_trace)
			{
				aiosidx = &sjob->aio[DSET_RIDX];
				sjob->st.events++;
				sjob->st.traces++;
			}
			else
			{
				aiosidx = &sjob->aio[DSET_MIDX];
				sjob->st.hists++;
			}
			sjob->cur_stream.idx.length = sjob->cur_stream.size;
			sjob->cur_stream.size = 0;
			sjob->cur_stream.cur_size = 0;

			jobrc = s_try_queue_aiobuf (aiosidx,
				(char*)&sjob->cur_stream.idx, SIDX_LEN);
			if (jobrc < 0)
				finishing = 1; /* error */
		}
	}
	else if (is_mca || is_trace)
	{ /* missed beginning of a stream or in the process of
	   * discarding */
		if ( ! interrupts_stream )
		{
			dbg_assert ( ! is_header );
			dbg_assert (sjob->cur_stream.size == 0);

			if ( ! sjob->cur_stream.discard )
			{
#if DEBUG_LEVEL >= LETS_GET_NUTS
				logmsg (0, LOG_DEBUG,
					"Received a non-header %s frame (#%lu) "
					"while no stream was ongoing",
					is_mca ? "histogram" : "trace",
					sjob->st.frames - 1);
#endif
				sjob->cur_stream.discard = true;
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
		{
			finishing = 1; /* DONE */
		}
	}
	else
	{ /* short event */
		/* FIX: check num events for dp trace */
		sjob->st.events += tespkt_event_nums (pkt);
	}

done:
	/* **************** Write frame payload. **************** */
	jobrc = s_try_queue_aiobuf (aiodat, datstart, datlen);
	if (jobrc < 0)
		finishing = 1; /* error */

	/* ***************** Write frame index. ***************** */

	jobrc = s_try_queue_aiobuf (
		aiofidx, (char*)&fidx, FIDX_LEN);
	if (jobrc < 0)
		finishing = 1; /* error */

	dbg_assert ( sjob->st.frames * FIDX_LEN ==
		aiofidx->size +
		aiofidx->bufzone.waiting +
		aiofidx->bufzone.enqueued );

	/* ********************* Check if done. ********************* */
	if (finishing)
	{
		/* Flush all buffers. */
		s_flush (sjob);

		logmsg (0, LOG_INFO,
			"Finished writing %lu ticks and %lu events",
			sjob->st.ticks, sjob->st.events);
#if DEBUG_LEVEL >= VERBOSE
		s_dbg_stats (sjob);
#endif
		/* Close stream and index files. */
		s_close (sjob);

		uint8_t status = ( ( sjob->min_ticks > sjob->st.ticks ||
			 sjob->min_events > sjob->st.events ) ?
			TES_CAP_REQ_EWRT : TES_CAP_REQ_OK );

		/* Write stats regardless of errors. */
		int rc = s_stats_write (sjob);
		if (status == TES_CAP_REQ_OK)
			status = rc;

		/* Convert them to hdf5, only if all is ok until now. */
		if ( status == TES_CAP_REQ_OK && ! sjob->noconvert )
			status = s_conv_data (sjob);

		/* Send reply. */
		s_stats_send (sjob, self->frontends[0].sock, status);

		/* Enable polling on the frontend and deactivate packet
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
task_cap_init (task_t* self)
{
	assert (self != NULL);
	assert (*(DATAROOT + strlen (DATAROOT) - 1) == '/');
	assert (sizeof (struct s_stats_t) == STAT_LEN);
	assert (sizeof (struct s_fidx_t) == FIDX_LEN);
	assert (sizeof (struct s_tidx_t) == TIDX_LEN);
	assert (sizeof (struct s_sidx_t) == SIDX_LEN);
	assert (sizeof (s_dsets) == NUM_DSETS * sizeof (struct s_dset_t));
	assert (memcmp (s_dsets[DSET_FIDX].extension, "fidx", 4) == 0);
	assert (memcmp (s_dsets[DSET_MIDX].extension, "midx", 4) == 0);
	assert (memcmp (s_dsets[DSET_TIDX].extension, "tidx", 4) == 0);
	assert (memcmp (s_dsets[DSET_RIDX].extension, "ridx", 4) == 0);
#ifdef SINGLE_FILE
	assert (memcmp (s_dsets[DSET_ADAT].extension, "adat", 4) == 0);
#else
	assert (memcmp (s_dsets[DSET_BDAT].extension, "bdat", 4) == 0);
	assert (memcmp (s_dsets[DSET_MDAT].extension, "mdat", 4) == 0);
	assert (memcmp (s_dsets[DSET_TDAT].extension, "tdat", 4) == 0);
	assert (memcmp (s_dsets[DSET_EDAT].extension, "edat", 4) == 0);
#endif

	assert (linear_etype (TESPKT_TYPE_PEAK, 0) == FTYPE_PEAK);
	assert (linear_etype (TESPKT_TYPE_AREA, 0) == FTYPE_AREA);
	assert (linear_etype (TESPKT_TYPE_PULSE, 0) == FTYPE_PULSE);
	assert (linear_etype (TESPKT_TYPE_TRACE, TESPKT_TRACE_TYPE_SGL) ==
		FTYPE_TRACE_SGL);
	assert (linear_etype (TESPKT_TYPE_TRACE, TESPKT_TRACE_TYPE_AVG) ==
		FTYPE_TRACE_AVG);
	assert (linear_etype (TESPKT_TYPE_TRACE, TESPKT_TRACE_TYPE_DP) ==
		FTYPE_TRACE_DP);
	assert (linear_etype (TESPKT_TYPE_TRACE, TESPKT_TRACE_TYPE_DPTR) ==
		FTYPE_TRACE_DPTR);

	static struct s_data_t sjob;
	sjob.statfd = -1;

	int rc = 0;
	for (int s = 0; s < NUM_DSETS ; s++)
	{
		rc = s_init_aiobuf (&sjob.aio[s]);
		if (rc != 0)
			break;
	}
	if (rc != 0)
	{
		logmsg (errno, LOG_ERR, "Cannot mmap %lu bytes", BUFSIZE);
		return -1;
	}

	self->data = &sjob;
	return 0;
}

/*
 * Send off stats for any ongoing job. Close all files.
 * Unmap data for stream and index files.
 * Returns 0 on success, -1 if job status could not be sent or
 * written.
 */
int
task_cap_fin (task_t* self)
{
	assert (self != NULL);

	struct s_data_t* sjob = (struct s_data_t*) self->data;
	assert (sjob != NULL);

	int rc = 0;
	if (sjob->basefname != NULL)
	{ /* A job is in progress. _stats_send nullifies this. */
		s_flush (sjob);
		s_close (sjob);
		rc  = s_stats_write (sjob);
		rc |= s_stats_send  (
			sjob, self->frontends[0].sock, TES_CAP_REQ_EWRT);
	}

	for (int s = 0; s < NUM_DSETS ; s++)
		s_fin_aiobuf (&sjob->aio[s]);

	self->data = NULL;
	return (rc ? -1 : 0);
}
