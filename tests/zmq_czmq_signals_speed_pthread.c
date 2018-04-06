#ifdef linux
/* ppoll requires it on linux */
#  define _GNU_SOURCE
#  include <signal.h>
#endif
#include <czmq.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>
#include <poll.h>
#include <sys/types.h>
#include "cutil.h"

/*
 * On FreeBSD 11.1, Intel Xeon E3-1275 @ 3.8GHz, CZMQ 4.1.1, gcc-6:
 * For WAIT_NSEC = x, avg bandwidth is y for ZMQ sockets and z for pipes
 *   x            y           z
 * 1 000 000: ~8.9*10^2  ~9.0*10^2 (as expected, upper limit is 10^3)
 * 100 000:   ~9.1*10^3  ~9.2*10^3 (as expected, upper limit is 10^4)
 * 10 000:    ~7.1*10^4  ~7.8*10^4 (some overhead, upper limit is 10^5)
 * Then it saturates:
 * 1 000:     ~2.3*10^5  ~2.8*10^5 (slow, upper limit is 10^6)
 * 100:       ~2.3*10^5  ~2.8*10^5
 * 10:        ~2.3*10^5  ~2.8*10^5
 * 1:         ~2.3*10^5  ~2.8*10^5
 *
 * Then skipping the call to ppoll at all (tight loop):
 * 0:         ~2.3*10^5  ~3.0*10^5
 *
 * So the ~2-3*10^5 is the max rate I can communicate with the
 * thread. Which is just slightly above the rate at which frames
 * come during trace measurements.
 *
 ******************************************************************
 *
 * On Linux 4.14, Intel Core i5-5287U @ 2.2 GHz, CZMQ 4.1.0, gcc-7:
 * For WAIT_NSEC = x, avg bandwidth is y for ZMQ sockets and z for pipes
 *   x       y           z
 * 1 000 000: ~9.0*10^2  ~9.1*10^2 (as expected, upper limit is 10^3)
 * 100 000:   ~5.7*10^3  ~5.7*10^3 (some overhead, upper limit is 10^4)
 * 10 000:    ~1.5*10^4  ~1.6*10^4 (slow, upper limit is 10^5)
 * Then it saturates for a while:
 * 1 000:     ~1.7*10^4  ~1.8*10^4
 * 100:       ~1.8*10^4  ~1.8*10^4
 * 10:        ~1.8*10^4  ~1.8*10^4
 * 1:         ~1.8*10^4  ~1.8*10^4
 *
 * Then skipping the call to ppoll at all (tight loop) does a lot
 * better, probably the call to ppoll is the limitting overhead:
 * 0:         ~5.3*10^5  ~7.6*10^5
 *
 * The rate during the tight loop is 3 times better here, even
 * though the CPU is 2/3 as powerful.
 * Very similar results with gcc-6.
 *
 */
#ifndef WAIT_NSEC
#define WAIT_NSEC   0LU
#endif
#ifndef MAX_SIGS
#define MAX_SIGS   5000000LU
#endif
// #define USE_PIPE

static bool busy;
struct pdata_t
{
	uint64_t signals;
	struct zmq_pollitem_t* pitem;
};

static int
s_wakeup (zmq_pollitem_t* pitem)
{
	if (busy)
		return 0;

#ifdef USE_PIPE /* pipe */
	int rc = write (pitem->fd, "0", 1);
#else /* zmq sock */
	int rc = zsock_signal (pitem->socket, 0);
#endif
	if (rc == -1)
	{
		perror ("Could not send signal");
		return -1;
	}
	return 0;
}

static int
s_sig_hn (zloop_t* loop, zmq_pollitem_t* pitem, void* pdata_)
{
	busy = true;
	struct pdata_t* pdata = (struct pdata_t*)pdata_;
#ifdef USE_PIPE /* pipe */
	char sig;
	int rc = read (pitem->fd, &sig, 1);
	assert (pitem->socket == NULL);
#else /* zmq sock */
	int rc = zsock_wait (pitem->socket);
#endif
	if (rc == -1)
	{
		perror ("Could not read signal");
		return -1;
	}
	pdata->signals++;

#if (WAIT_NSEC > 500000000)
	printf ("Got a signal\n");
#endif
	busy = false;
	return 0;
}

static int
s_terminator (zloop_t* loop, zsock_t* sock, void* arg_)
{
	printf ("Terminating\n");
	return -1;
}

static void
s_task_shim (zsock_t* pipe, void* pdata_)
{
	zloop_t* loop = zloop_new ();
	struct pdata_t* pdata = (struct pdata_t*)pdata_;
	int rc = zloop_poller (loop, pdata->pitem, s_sig_hn, pdata_);
	if (rc == 0)
		rc = zloop_reader (loop, pipe, s_terminator, NULL);
	if (rc != 0)
		fprintf (stderr, "Task: Could not register the zloop readers\n");
	else
	{
		zsock_signal (pipe, 0); /* zactor_new will wait for this */
		zloop_start (loop);
	}

	zloop_destroy (&loop);
}

int
main (void)
{
	struct zmq_pollitem_t master = {0};
	master.events = ZMQ_POLLOUT;
	struct zmq_pollitem_t slave = {0};
	slave.events = ZMQ_POLLIN;
	zsys_init ();
	int rc;

	/* Create the sockets. */
#ifdef USE_PIPE /* pipe */
	printf ("Using a pipe fd\n");
	int pipefds[2];
	rc = pipe(pipefds);
	if (rc != 0)
	{
		fprintf (stderr, "Cannot create pipe\n");
		return -1;
	}
	slave.fd = pipefds[0];
	master.fd = pipefds[1];
	assert (slave.socket == NULL);
	assert (master.socket == NULL);
#else /* zmq sock */
	printf ("Using a ZMQ sock\n");
	zsock_t* mastersock = zsock_new_pair ("@inproc://pipe");
	zsock_t* slavesock = zsock_new_pair (">inproc://pipe");
	if (mastersock == NULL || slavesock == NULL)
	{
		fprintf (stderr, "Cannot create pair sockets\n");
		zsock_destroy (&mastersock);
		zsock_destroy (&slavesock);
		return -1;
	}
	zsock_set_sndtimeo (mastersock, 100);
	zsock_set_rcvtimeo (slavesock, 100);
	slave.socket = zsock_resolve (slavesock);
	master.socket = zsock_resolve (mastersock);
	assert (slave.socket != NULL);
	assert (master.socket != NULL);
#endif
	printf ("Sleeping for %lu ns every loop.\n",
		WAIT_NSEC);

	/* Catch interrupts, set zsys_interrupted. */
	zsys_catch_interrupts ();

	/* Create the task thread. */
	struct pdata_t pdata = {
		.signals = 0,
		.pitem = &slave
		};
	zactor_t* task = zactor_new (s_task_shim, &pdata);

	/* Enter loop. */
	uint64_t loops = 0;
	struct timespec ts;
	tic (&ts);
	rc = 0;
#if (WAIT_NSEC > 0)
	uint32_t skipped_polls = 0;
	struct timespec twait = {
		.tv_sec = 0,
		.tv_nsec = WAIT_NSEC
	};
#endif
	while (!zsys_interrupted && rc == 0 &&
		pdata.signals < MAX_SIGS)
	{
#if (WAIT_NSEC > 0)
		ppoll (NULL, 0, &twait, NULL);
#endif
		rc = s_wakeup (&master);
		loops++;
	}

	long long nsecs = toc (&ts);
	printf ("loops:   %lu\nsignals: %lu\navg speed: %.5e lps\n",
			loops, pdata.signals,
			(double)pdata.signals * NSEC_IN_SEC / nsecs);

	printf ("Destroying thread\n");

    /* FIX: thread is not listening for term signal */
	zactor_destroy (&task);
#ifndef USE_PIPE /* zmq sock */
	zsock_destroy (&slavesock);
	zsock_destroy (&mastersock);
#endif
	if (zsys_interrupted || rc)
		return -1;
	return 0;
}
