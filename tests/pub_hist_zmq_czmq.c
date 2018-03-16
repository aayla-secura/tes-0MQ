#include <czmq.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <assert.h>
#include "net/tespkt.h"

#define PUBADDR "@tcp://*:55565"
#define WAIT_MSEC 1000
#define ADD_NOISE
#define NBINS ((TES_HIST_MAXSIZE - TESPKT_MCA_HDR_LEN) / TESPKT_MCA_BIN_LEN)

struct data_t
{
	zsock_t* sock;
	int fd;
};

static int
send_hist (zloop_t* loop, int tID, void* data_)
{
	struct data_t* data = (struct data_t*)data_;
	char hist[TES_HIST_MAXSIZE] = {0};
	ssize_t wrc = read (data->fd, hist, TES_HIST_MAXSIZE);
	if (wrc == 0)
	{
		if (lseek (data->fd, 0, SEEK_SET) != 0)
		{
			fprintf (stderr, "Cannot seek to BOF\n");
			return -1;
		}
		/* printf ("Starting over\n"); */
		return send_hist (loop, tID, data_);
	}
	if (wrc == -1 || (size_t)wrc != TES_HIST_MAXSIZE)
	{
		if (wrc == -1)
			perror ("read");
		else
			fprintf (stderr, "Read %lu bytes\n", wrc);
		return -1;
	}

#ifdef ADD_NOISE
	uint32_t* bin_p = (uint32_t*)(hist + TESPKT_MCA_HDR_LEN);
	for (uint16_t bin_n = 0; bin_n < NBINS; bin_n++, bin_p++)
	{
		*bin_p += (int) (*bin_p * ((double)rand()/RAND_MAX - 0.5));
	}
	assert ((void*)bin_p == (void*)(hist + TES_HIST_MAXSIZE));
#endif
	int rc = zmq_send (zsock_resolve (data->sock),
		hist, TES_HIST_MAXSIZE, 0);

	return rc;
}

static int
new_subscriber (zloop_t* loop, zsock_t* sock, void* arg)
{
	zmsg_t* msg = zmsg_recv (sock);
	if (msg == NULL)
		return -1;

	printf ("Got a %lu-frame message\n", zmsg_size(msg));
	zframe_t* frame = zmsg_first (msg);
	while (frame != NULL)
	{
		char* fh = zframe_strhex (frame);
		assert (fh != NULL);
		printf ("Frame (%lu bytes long): %s\n", strlen (fh), fh);
		zstr_free (&fh);
		frame = zmsg_next (msg);
	}

	zmsg_destroy (&msg);
	return 0;
}

int
main (int argc, char* argv[])
{
	struct data_t data = {0,};
	if (argc != 2)
		return -1;

	data.fd = open (argv[1], O_RDONLY);
	if (data.fd == -1)
	{
		perror ("open");
		return -1;
	}
	off_t hlen = lseek (data.fd, 0, SEEK_END);
	if (hlen == (off_t)-1 || (size_t)hlen % TES_HIST_MAXSIZE != 0)
	{
		fprintf (stderr,
			"File size is not a multiple of histogram size\n");
		return -1;
	}

	data.sock = zsock_new_xpub (PUBADDR);
	if (data.sock == NULL)
	{
		perror ("zsock_new");
		close(data.fd);
		return -1;
	}
	zloop_t* loop = zloop_new ();
	if (loop == NULL)
	{
		perror ("zloop_new");
		close(data.fd);
		zsock_destroy (&data.sock);
		return -1;
	}

	zloop_reader (loop, data.sock, new_subscriber, NULL);
	zloop_timer (loop, WAIT_MSEC, 0, send_hist, &data);
	int rc = zloop_start (loop);

	close(data.fd);
	zsock_destroy (&data.sock);
	zloop_destroy (&loop);
	return rc;
}
