/* TO DO: test with mmapped files, in daemon mode */

#include "hdf5conv.h"
#include "daemon_ng.h"
#include "sys/types.h"
#include "sys/stat.h"
#include "sys/mman.h"
#include "stdint.h"
#include "syslog.h"
#include "fcntl.h"
#include "unistd.h"
#include "errno.h"

#define DATAFILE "/media/data/testcap"
#define H5FNAME "/media/data/test.hdf5"
#define MEASUREMENT "foo"
// #define OVRWTMODE HDF5_OVRWT_NONE
#define OVRWTMODE HDF5_OVRWT_RELINK
// #define OVRWTMODE HDF5_OVRWT_FILE
#define ASYNC 0
#define DAEMONIZE 0

int main (void)
{
	set_verbose (1);

	/* Open the data file. */
	int fd = open (DATAFILE, O_RDONLY);
	if (fd == -1)
	{
		logmsg (errno, LOG_ERR,
			"Could not open data file %s",
			DATAFILE);
		return -1;
	}

	off_t fsize = lseek (fd, 0, SEEK_END);
	if (fsize == (off_t)-1)
	{
		logmsg (errno, LOG_ERR,
			"Could not seek to end of file %s",
			DATAFILE);
		close (fd);
		return -1;
	}
	
	/* mmap from BOF, since mmap requires the offset be a multiple
	 * of page size. */
	char* data = (char*)mmap (NULL, fsize,
		PROT_READ, MAP_PRIVATE, fd, 0);
	if ((void*)data == (void*)-1)
	{
		logmsg (errno, LOG_ERR,
			"Could not mmap file %s",
			DATAFILE);
		close (fd);
		return -1;
	}
	struct hdf5_dset_desc_t dsets[] = {
		{ /* mmapped stream */
			.buffer = data + 1,
			.dsetname = "mmap",
			.length = fsize - 1, /* all but first byte */
		},
		{ /* file stream */
			.filename = DATAFILE,
			.dsetname = "fileA",
			.offset = fsize,
			.length = fsize, /* empty */
		},
		{ /* file stream */
			.filename = DATAFILE,
			.dsetname = "fileB",
			.offset = fsize + 1,
			.length = fsize, /* empty */
		},
		{ /* file stream */
			.filename = DATAFILE,
			.dsetname = "fileC",
			.offset = -3, /* last 3 bytes */
			.length = fsize,
		},
		{ /* file stream */
			.filename = DATAFILE,
			.dsetname = "fileD",
			.offset = -3, /* last 3 bytes */
			.length = -1,
		},
		{ /* file stream */
			.filename = DATAFILE,
			.dsetname = "fileE",
			.offset = fsize - 3, /* last 3 bytes */
			.length = fsize,
		},
		{ /* file stream */
			.filename = DATAFILE,
			.dsetname = "fileF",
			.offset = -fsize - 1, /* empty */
			.length = 1,
		},
		{ /* file stream */
			.filename = DATAFILE,
			.dsetname = "fileG",
			.offset = 2,
			.length = fsize, /* all but first 2 bytes */
		},
		{ /* file stream */
			.filename = DATAFILE,
			.dsetname = "fileH",
			.offset = 0,
			.length = fsize + 1, /* entire file */
		},
	};
	uint8_t num_dsets = sizeof (dsets) / sizeof (
		struct hdf5_dset_desc_t);

	struct hdf5_conv_req_t creq = {
		.filename = H5FNAME,
		.group = MEASUREMENT,
		.dsets = dsets,
		.num_dsets = num_dsets,
		.ovrwtmode = OVRWTMODE,
		.async = ASYNC,
	};

	int rc = 0;
#if DAEMONIZE
	rc = daemonize (NULL, NULL, NULL, 0);
#endif
	if (rc == 0)
		rc = hdf5_conv (&creq);

	return rc;
}
