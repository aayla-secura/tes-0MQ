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

#define BASEFNAME "/media/data/testcap"
#define H5FNAME "/media/data/test.hdf5"
// #define MEASUREMENT "measurement"
#define MEASUREMENT ""
#define OVRWRT 1
#define ASYNC 0
#define DAEMONIZE 0

int main (void)
{
	set_verbose (1);

	/* Open the data file. */
	int fd = open (BASEFNAME ".tdat", O_RDONLY);
	if (fd == -1)
	{
		logmsg (errno, LOG_ERR,
			"Could not open data file %s.tdat",
			BASEFNAME);
		return -1;
	}

	off_t fsize = lseek (fd, 0, SEEK_END);
	if (fsize == (off_t)-1)
	{
		logmsg (errno, LOG_ERR,
			"Could not seek to end of file %s.tdat",
			BASEFNAME);
		close (fd);
		return -1;
	}
	
	/* mmap from BOF, since mmap requires the offset be a multiple of
	 * page size. */
	char* data = (char*)mmap (NULL, fsize,
			PROT_READ, MAP_PRIVATE, fd, 0);
	if ((void*)data == (void*)-1)
	{
		logmsg (errno, LOG_ERR,
			"Could not mmap file %s.tdat",
			BASEFNAME);
		close (fd);
		return -1;
	}
	uint8_t num_dsets = 2;
	struct hdf5_dset_desc_t dsets[] = {
		{ /* tick stream */
			.buffer = data + 2,
			.dname = "tick",
			.offset = -1,
			.length = 6,
		},
		{ /* event stream */
			.filename = BASEFNAME ".edat",
			.dname = "event",
			.offset = 2,
			.length = 6,
		},
	};
	assert (num_dsets == sizeof (dsets) / sizeof (struct hdf5_dset_desc_t));

	struct hdf5_conv_req_t creq = {
		.filename = H5FNAME,
		.group = MEASUREMENT,
		.datasets = dsets,
		.num_dsets = num_dsets,
		.ovrwt = OVRWRT,
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
