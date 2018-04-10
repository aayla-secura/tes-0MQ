/* TODO: test with mmapped files, in daemon mode */

#include "hdf5conv.h"
#include "api.h"
#include "daemon_ng.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdint.h>
#include <syslog.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define DATAFILE "/media/data/test.dat"
#define H5FNAME "/media/data/test.hdf5"
#define MEASUREMENT "foo"
#define ASYNC false
#define DAEMONIZE false
#define DATALEN 16

int main (void)
{
	set_verbose (1);
#if DAEMONIZE
	if (daemonize (NULL, NULL, NULL, 0) != 0)
    return -1;
#endif

  /* Create the data file. */
	int fd = open (DATAFILE, O_CREAT | O_TRUNC | O_RDWR,
    S_IRUSR | S_IWUSR);
	if (fd == -1)
	{
		logmsg (errno, LOG_ERR,
			"Could not open data file %s", DATAFILE);
		return -1;
	}
  char buf[DATALEN] = {0};
  for (int i = 0; i < DATALEN; i++)
    buf[i] = i;
  ssize_t wrc = write (fd, buf, DATALEN);
  if (wrc == -1)
  {
    logmsg (errno, LOG_ERR,
      "Cannot write to data file %s", DATAFILE);
		close (fd);
    return -1;
  }

#ifdef USE_MMAP
	/* mmap from BOF, since mmap requires the offset be a multiple
	 * of page size. */
	char* data = (char*)mmap (NULL, DATALEN,
		PROT_READ, MAP_PRIVATE, fd, 0);
	if ((void*)data == (void*)-1)
	{
		logmsg (errno, LOG_ERR,
			"Could not mmap file %s", DATAFILE);
		close (fd);
		return -1;
	}
#else
  close (fd);
#endif

	struct hdf5_dset_desc_t dsets[] = {
		{ /* empty */
#ifdef USE_MMAP
			.buffer = data,
#else
			.filename = DATAFILE,
#endif
			.dsetname = "file: start = LEN, len = LEN",
			.offset = DATALEN,
			.length = DATALEN,
		},
		{ /* empty */
#ifdef USE_MMAP
			.buffer = data,
#else
			.filename = DATAFILE,
#endif
			.dsetname = "file: start = LEN+1, len = LEN",
			.offset = DATALEN + 1,
			.length = DATALEN,
		},
		{ /* last 3 bytes */
#ifdef USE_MMAP
			.buffer = data,
#else
			.filename = DATAFILE,
#endif
			.dsetname = "file: start = END-3, len = LEN",
			.offset = -3,
			.length = DATALEN,
		},
		{ /* last 3 bytes */
#ifdef USE_MMAP
			.buffer = data,
#else
			.filename = DATAFILE,
#endif
			.dsetname = "file: start = END-3, len = ALL",
			.offset = -3,
			.length = -1,
		},
		{ /* last 3 bytes */
#ifdef USE_MMAP
			.buffer = data,
#else
			.filename = DATAFILE,
#endif
			.dsetname = "file: start = LEN-3, len = LEN",
			.offset = DATALEN - 3,
			.length = DATALEN,
		},
		{ /* empty */
#ifdef USE_MMAP
			.buffer = data,
#else
			.filename = DATAFILE,
#endif
			.dsetname = "file: start = -LEN-1, len = 1",
			.offset = -DATALEN - 1,
			.length = 1,
		},
		{ /* all but first 2 bytes */
#ifdef USE_MMAP
			.buffer = data,
#else
			.filename = DATAFILE,
#endif
			.dsetname = "file: start = 2, len = LEN",
			.offset = 2,
			.length = DATALEN,
		},
		{ /* entire file */
#ifdef USE_MMAP
			.buffer = data,
#else
			.filename = DATAFILE,
#endif
			.dsetname = "file: start = 0, len = LEN+1",
			.offset = 0,
			.length = DATALEN + 1,
		},
	};
	uint8_t num_dsets =
    sizeof (dsets) / sizeof (struct hdf5_dset_desc_t);

  /* ---------------------------------------------------------- */
  logmsg (0, LOG_INFO, "------------------------------");
  logmsg (0, LOG_INFO,
    "use_existing = false, overwrite = true, backup = false\n");
  {
    struct hdf5_conv_req_t creq = {
      .filename = H5FNAME,
      .group = MEASUREMENT,
      .dsets = dsets,
      .num_dsets = num_dsets,
      .use_existing = false,
      .overwrite = true,
      .backup = false,
      .async = ASYNC,
    };
    if (hdf5_conv (&creq) != HDF5CONV_REQ_OK)
    {
      close (fd);
      return -1;
    }
  }

  /* ---------------------------------------------------------- */
  logmsg (0, LOG_INFO, "------------------------------");
  logmsg (0, LOG_INFO,
    "use_existing = false, overwrite = true, backup = true\n");
  {
    struct hdf5_conv_req_t creq = {
      .filename = H5FNAME,
      .group = MEASUREMENT,
      .dsets = dsets,
      .num_dsets = num_dsets,
      .use_existing = false,
      .overwrite = true,
      .backup = true,
      .async = ASYNC,
    };
    if (hdf5_conv (&creq) != HDF5CONV_REQ_OK)
    {
      close (fd);
      return -1;
    }
  }

  /* ---------------------------------------------------------- */
  logmsg (0, LOG_INFO, "------------------------------");
  logmsg (0, LOG_INFO,
    "use_existing = false, overwrite = false, backup = false\n");
  {
    struct hdf5_conv_req_t creq = {
      .filename = H5FNAME,
      .group = MEASUREMENT,
      .dsets = dsets,
      .num_dsets = num_dsets,
      .use_existing = false,
      .overwrite = false,
      .backup = false,
      .async = ASYNC,
    };
    if (hdf5_conv (&creq) != HDF5CONV_REQ_EABORT)
    {
      close (fd);
      return -1;
    }
  }

  /* ---------------------------------------------------------- */
  /* ---------------------------------------------------------- */
  logmsg (0, LOG_INFO, "------------------------------");
  logmsg (0, LOG_INFO,
    "use_existing = true, overwrite = true, backup = false\n");
  {
    struct hdf5_conv_req_t creq = {
      .filename = H5FNAME,
      .group = MEASUREMENT,
      .dsets = dsets,
      .num_dsets = num_dsets,
      .use_existing = true,
      .overwrite = true,
      .backup = false,
      .async = ASYNC,
    };
    if (hdf5_conv (&creq) != HDF5CONV_REQ_OK)
    {
      close (fd);
      return -1;
    }
  }

  /* ---------------------------------------------------------- */
  logmsg (0, LOG_INFO, "------------------------------");
  logmsg (0, LOG_INFO,
    "use_existing = true, overwrite = true, backup = true\n");
  {
    struct hdf5_conv_req_t creq = {
      .filename = H5FNAME,
      .group = MEASUREMENT,
      .dsets = dsets,
      .num_dsets = num_dsets,
      .use_existing = true,
      .overwrite = true,
      .backup = true,
      .async = ASYNC,
    };
    if (hdf5_conv (&creq) != HDF5CONV_REQ_OK)
    {
      close (fd);
      return -1;
    }
  }

  /* ---------------------------------------------------------- */
  logmsg (0, LOG_INFO, "------------------------------");
  logmsg (0, LOG_INFO,
    "use_existing = true, overwrite = false, backup = false\n");
  {
    struct hdf5_conv_req_t creq = {
      .filename = H5FNAME,
      .group = MEASUREMENT,
      .dsets = dsets,
      .num_dsets = num_dsets,
      .use_existing = true,
      .overwrite = false,
      .backup = false,
      .async = ASYNC,
    };
    if (hdf5_conv (&creq) != HDF5CONV_REQ_EABORT)
    {
      close (fd);
      return -1;
    }
  }

  close (fd);
	return 0;
}
