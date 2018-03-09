#include <czmq.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <assert.h>
#include "net/tespkt.h"

#define PUBADDR "@tcp://*:55565"
#define WAIT_MSEC 200
#define ADD_NOISE

int main (int argc, char** argv)
{
	if (argc != 2)
		return -1;
	zsys_init ();
	zsys_catch_interrupts ();

	int fd = open (argv[1], O_RDONLY);
	if (fd == -1)
	{
		perror ("open");
		return -1;
	}
	off_t hlen = lseek (fd, 0, SEEK_END);
	if (hlen == (off_t)-1 || (size_t)hlen % TES_HIST_MAXSIZE != 0)
	{
		fprintf (stderr,
			"File size is not a multiple of histogram size\n");
		return -1;
	}

	zsock_t* frontend = zsock_new_pub (PUBADDR);
	if (frontend == NULL)
	{
		perror ("zsock_new");
		return -1;
	}
	int rc = 0;
	uint16_t nbins = (TES_HIST_MAXSIZE - MCA_HDR_LEN) / BIN_LEN;
	while (!zsys_interrupted)
	{
		char hist[TES_HIST_MAXSIZE] = {0};
		ssize_t wrc = read (fd, hist, TES_HIST_MAXSIZE);
		if (wrc == 0)
		{
			if (lseek (fd, 0, SEEK_SET) != 0)
			{
				fprintf (stderr, "Cannot seek to BOF\n");
				rc = -1;
				break;
			}
			continue;
		}
		if (wrc == -1 || (size_t)wrc != TES_HIST_MAXSIZE)
		{
			if (wrc == -1)
				perror ("read");
			else
				fprintf (stderr, "Read %lu bytes\n", wrc);
			rc = -1;
			break;
		}

#ifdef ADD_NOISE
		uint32_t* bin_p = (uint32_t*)(hist + MCA_HDR_LEN);
		for (uint16_t bin_n = 0; bin_n < nbins; bin_n++, bin_p++)
		{
			*bin_p += (int) (*bin_p * ((double)rand()/RAND_MAX - 0.5));
		}
		assert ((void*)bin_p == (void*)(hist + TES_HIST_MAXSIZE));
#endif
		rc = zmq_send (zsock_resolve (frontend),
			hist, TES_HIST_MAXSIZE, 0);
		if (rc == -1)
			break;

		poll (NULL, 0, WAIT_MSEC);
	}

	zsock_destroy (&frontend);
	return rc;
}
