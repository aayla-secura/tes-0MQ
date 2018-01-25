#define _WITH_GETLINE
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <assert.h>
#include <czmq.h>

#ifndef PATH_MAX
#  ifdef MAXPATHLEN
#    define PATH_MAX MAXPATHLEN
#  else
#    define PATH_MAX 4096
#  endif
#endif

/* mode A */
#define A_REQ_OK    0 // accepted
#define A_REQ_INV   1 // malformed request
#define A_REQ_ABORT 2 // no such job (for status query) or file exist (for no-overwrite)
#define A_REQ_EPERM 3 // filename is not allowed
#define A_REQ_FAIL  4 // other error opening the file, nothing was written
#define A_REQ_EWRT  5 // error while writing, less than minimum requested was saved
#define A_REQ_ECONV 6 // error while converting to hdf5
#define A_REQ_PIC   "ss8811"
#define A_REP_PIC "18888888"

/* mode T */
#define T_REQ_OK    0 // accepted
#define T_REQ_INV   1 // malformed request
#define T_REQ_TOUT  2 // timeout error
#define T_REQ_PIC  "4"
#define T_REP_PIC "1c"
#define MAX_TRACE_SIZE 65528U

/* mode H */
#if 0 /* FIX */
#define MAX_HIST_SIZE 65528U
#else
#define MAX_HIST_SIZE 65576U
#endif

int interrupted;

static void
usage (const char* self)
{
	fprintf (stdout,
		"Usage: %s -Z <server> [options]\n"
		"The format for <server> is <proto>://<host>:<port>\n\n"
		"The client operates in one of three modes:\n"
		"1) Save frames to a remote file: Selected by the 'A' option.\n"
		"  Options:\n"
		"    -A <filename>      Remote filename.\n"
		"    -m <measurement>   Measurement name. Default is empty.\n"
		"    -t <ticks>         Save at least that many ticks.\n"
		"                       Default is 1.\n"
		"    -e <ticks>         Save at least that many non-tick\n"
	       	"                       events. Default is 0.\n"
		"    -o                 Overwrite if file exists.\n"
		"    -s                 Request status of filename.\n"
		"    -a                 Asynchronous hdf5 conversion.\n"
		"The 'o', 't' or 'e' options cannot be given for status\n"
	        "                       requests.\n\n"
		"2) Save average traces to a local file: Selected by the 'T' option.\n"
		"  Options:\n"
		"    -T <filename>      Local filename. Will append if\n"
		"                       existing.\n"
		"    -w <timeout>       Timeout in seconds. Sent to the server, will\n"
		"                       receive a timeout error if no trace arrives\n"
		"                       in this period.\n"
		"3) Save histograms to a local file: Selected by the 'H' option.\n"
		"  Options:\n"
		"    -H <filename>      Local filename. Will append if\n"
		"                       existing.\n"
		"    -c <count>         Save up to that many histograms.\n"
		"                       Default is 1.\n\n",
		self
		);
	exit (EXIT_SUCCESS);
}

static int
prompt (void)
{
	printf ("\nProceed (y/n)? ");
	do
	{
		char* line = NULL;
		size_t len = 0;
		ssize_t rlen = getline (&line, &len, stdin);

		if (line == NULL || rlen == -1)
			return -1;
		char rep = line[0];
		free (line);
		if (rlen == 2)
		{
			if (rep == 'n' || rep == 'N')
				return -1;
			if (rep == 'y' || rep == 'Y')
				return 0;
		}

		printf ("Reply with 'y' or 'n': ");
	} while (1);
}

static void
int_hn (int sig)
{
	interrupted = 1;
}

static int
save_trace (const char* server, const char* filename, uint32_t timeout)
{
	/* Open the socket */
	errno = 0;
	zsock_t* sock = zsock_new_req (server);
	if (sock == NULL)
	{
		if (errno)
			perror ("Could not connect to the server");
		else
			fprintf (stderr, "Could not connect to the server\n");
		return -1;
	}

	/* Open the file */
	int fd = open (filename, O_RDWR | O_APPEND | O_CREAT,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (fd == -1)
	{
		perror ("Could not open the file");
		zsock_destroy (&sock);
		return -1;
	}
	off_t fsize = lseek (fd, 0, SEEK_END);
	if (fsize == (off_t)-1)
	{
		perror ("Could not seek to end of file");
		close (fd);
		zsock_destroy (&sock);
		return -1;
	}
	if (fsize > 0)
		printf ("Appending to file of size %lu\n", fsize);

	/* Send the request */
	zsock_send (sock, T_REQ_PIC, timeout);
	puts ("Waiting for reply");

	uint8_t rep;
	zchunk_t* trace;
	int rc = zsock_recv (sock, T_REP_PIC, &rep, &trace);
	zsock_destroy (&sock);

	if (rc == -1)
		return -1;

	size_t trsize = 0;

	/* Print reply */
	printf ("\n");
	switch (rep)
	{
		case T_REQ_INV:
			printf ("Request was not understood\n");
			break;
		case T_REQ_TOUT:
			printf ("Request timed out\n");
			break;
		case T_REQ_OK:
			trsize = zchunk_size (trace);
			printf ("Received %lu bytes of data\n",
				trsize);
			break;
		default:
			assert (0);
	}

	/* Write to file */
	if (trsize == 0)
	{
		close (fd);
		return 0;
	}
	
	ssize_t wrc = write (fd, zchunk_data (trace), trsize);
	if (wrc == -1)
	{
		perror ("Could not write to file");
	}
	else if ((size_t)wrc != trsize)
	{
		fprintf (stderr, "Wrote only %lu bytes\n", wrc);
	}

	close (fd);
	return 0;
}

static int
save_hist (const char* server, const char* filename, uint64_t cnt)
{
	/* Open the socket */
	errno = 0;
	zsock_t* sock = zsock_new_sub (server, "");
	if (sock == NULL)
	{
		if (errno)
			perror ("Could not connect to the server");
		else
			fprintf (stderr, "Could not connect to the server\n");
		return -1;
	}

	/* Open the file */
	int fd = open (filename, O_RDWR | O_APPEND | O_CREAT,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (fd == -1)
	{
		perror ("Could not open the file");
		zsock_destroy (&sock);
		return -1;
	}
	off_t fsize = lseek (fd, 0, SEEK_END);
	if (fsize == (off_t)-1)
	{
		perror ("Could not seek to end of file");
		close (fd);
		zsock_destroy (&sock);
		return -1;
	}
	if (fsize > 0)
		printf ("Appending to file of size %lu\n", fsize);

	/* Allocate space */
	int rc = posix_fallocate (fd, fsize, cnt*MAX_HIST_SIZE);
	if (rc)
	{
		errno = rc; /* posix_fallocate does not set it */
		perror ("Could not allocate sufficient space");
		close (fd);
		zsock_destroy (&sock);
		return -1;
	}

	/* mmap it */
	/* TO DO: map starting at the last page boundary before end of file */
	unsigned char* map = (unsigned char*)mmap (NULL,
		fsize + cnt*MAX_HIST_SIZE, PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == (void*)-1)
	{
		perror ("Could not mmap file");
		close (fd);
		zsock_destroy (&sock);
		return -1;
	}

	uint64_t h = 0;
	size_t hsize = 0;
	void* sock_h = zsock_resolve (sock);
	assert (sock_h != NULL);
	for (; ! interrupted && h < cnt; h++)
	{
		rc = zmq_recv (sock_h, map + fsize + hsize, MAX_HIST_SIZE, 0);
		if (rc == -1)
		{
			perror ("Could not write to file");
			break;
		}
		else if ((size_t)rc > MAX_HIST_SIZE)
		{
			fprintf (stderr, "Frame is too large: %lu bytes", (size_t)rc);
			break;
		}
		hsize += (size_t)rc;
	}
	if (h < cnt - 1)
		printf ("Saved %lu histograms\n", h);

	zsock_destroy (&sock);
	munmap (map, cnt*MAX_HIST_SIZE);
	rc = ftruncate (fd, fsize + hsize);
	if (rc == -1)
		perror ("Could not truncate file");
	close (fd);
	return 0;
}

static int
save_to_remote (const char* server, const char* filename, const char* measurement,
		uint64_t min_ticks, uint64_t min_events, uint8_t ovrwrt, uint8_t async)
{
	/* Open the socket */
	errno = 0;
	zsock_t* sock = zsock_new_req (server);
	if (sock == NULL)
	{
		if (errno)
			perror ("Could not connect to the server");
		else
			fprintf (stderr, "Could not connect to the server\n");
		return -1;
	}

	/* Send the request */
	zsock_send (sock, A_REQ_PIC,
			filename,
			measurement,
			min_ticks,
			min_events,
			ovrwrt,
			async);
	puts ("Waiting for reply");

	uint8_t fstat;
	uint64_t ticks, events, traces, hists, frames, missed, dropped;
	int rc = zsock_recv (sock, A_REP_PIC,
				&fstat,
				&ticks,
				&events,
				&traces,
				&hists,
				&frames,
				&missed,
				&dropped); 
	zsock_destroy (&sock);

	if (rc == -1)
		return -1;

	/* Print reply */
	printf ("\n");
	switch (fstat)
	{
		case A_REQ_INV:
			printf ("Request was not understood\n");
			break;
		case A_REQ_ABORT:
			printf ("File %s\n", min_ticks ?
				"exists" : "does not exist");
			break;
		case A_REQ_EPERM:
			printf ("Filename is not allowed\n");
			break;
		case A_REQ_FAIL:
			printf ("Unknown error while opening\n\n");
			break;
		case A_REQ_EWRT:
			printf ("Unknown error while writing\n\n");
			/* fallthrough */
		case A_REQ_ECONV:
			printf ("Unknown error while converting\n\n");
			/* fallthrough */
		case A_REQ_OK:
			printf ("%s\n"
				"ticks:          %lu\n"
				"other events:   %lu\n"
				"traces:         %lu\n"
				"histograms:     %lu\n"
				"saved frames:   %lu\n"
				"missed frames:  %lu\n"
				"dropped frames: %lu\n",
				min_ticks ? "Wrote" : "File contains",
				ticks, events, traces, hists, frames, missed, dropped);
			break;
		default:
			assert (0);
	}

	return 0;
}

	int
main (int argc, char **argv)
{
	int rc;

	/* Signal handlers */
	struct sigaction sigact;
	sigact.sa_flags = 0;
	sigact.sa_handler = int_hn;
	sigemptyset (&sigact.sa_mask);
	sigaddset (&sigact.sa_mask, SIGINT);
	sigaddset (&sigact.sa_mask, SIGTERM);
	rc  = sigaction (SIGINT, &sigact, NULL);
	rc |= sigaction (SIGTERM, &sigact, NULL);
	if (rc == -1)
	{
		perror ("sigaction");
		exit (EXIT_FAILURE);
	}

	/* Command-line */
	char server[1024];
	memset (server, 0, sizeof (server));
	char filename[PATH_MAX];
	memset (filename, 0, sizeof (filename));
	char measurement[1024];
	memset (measurement, 0, sizeof (measurement));
	char* buf = NULL;
	uint64_t min_ticks = 0, min_events = 0, num_hist = 0;
	uint32_t timeout = 0;
	uint8_t ovrwrt = 0, async = 0, status = 0;
	/* A for remotely save all frames, T, for locally save traces, H for
	 * locally save histograms. */
	char mode = '\0';

	int opt;
	while ( (opt = getopt (argc, argv, "Z:H:T:A:m:w:c:t:e:osah")) != -1 )
	{
		switch (opt)
		{
			case 'Z':
				snprintf (server, sizeof (server),
						"%s", optarg);
				break;
			case 'A':
			case 'T':
			case 'H':
				snprintf (filename, sizeof (filename),
						"%s", optarg);
				mode = opt;
				break;
			case 'm':
				snprintf (measurement, sizeof (measurement),
						"%s", optarg);
				break;
			case 'w':
				timeout = strtoul (optarg, &buf, 10);
			case 'c':
				if (opt == 'c')
					num_hist = strtoul (optarg, &buf, 10);
			case 't':
				if (opt == 't')
					min_ticks = strtoul (optarg, &buf, 10);
			case 'e':
				if (opt == 'e')
					min_events = strtoul (optarg, &buf, 10);

				if (strlen (buf))
				{
					fprintf (stderr, "Invalid format for "
						"option %c.\n", opt);
					// usage (argv[0]);
					exit (EXIT_FAILURE);
				}
				break;
			case 'o':
				ovrwrt = 1;
				break;
			case 's':
				status = 1;
				break;
			case 'a':
				async = 1;
				break;
			case 'h':
			case '?':
				usage (argv[0]);
				break;
			default:
				/* we forgot to handle an option */
				assert (0);
		}
	}

	if (argc > optind)
	{
		fprintf (stderr, "Extra arguments given.\n"
			"Type %s -h for help\n", argv[0]);
		exit (EXIT_FAILURE);
	}

	/* Handle missing mandatory options. */
	if (strlen (server) == 0)
	{
		fprintf (stderr, "You must specify the remote address.\n"
			"Type %s -h for help\n", argv[0]);
		exit (EXIT_FAILURE);
	}
	if (strlen (filename) == 0)
	{
		fprintf (stderr, "You must specify a filename.\n"
			"Type %s -h for help\n", argv[0]);
		exit (EXIT_FAILURE);
	}
	/* if filename was given, mode should have been set */
	assert (mode != '\0');

	/* Handle conflicting options and set defaults. */
	/* Then, prompt and take action. */
	switch (mode)
	{
		case 'A':
			/* save frames to remote file */
			if ( num_hist || timeout ||
				( status && (async || ovrwrt || min_ticks || min_events) ) )
			{
				fprintf (stderr, "Conflicting options.\n"
					"Type %s -h for help\n", argv[0]);
				exit (EXIT_FAILURE);
			}
			if ( ! min_ticks && ! status )
				min_ticks = 1;

			/* Proceed? */
			printf ("Sending %s%s request for remote filename %s and group %s.",
				async ? "an asynchronous " : "a ",
				status ? "status": (ovrwrt ? "overwrite" : "write" ),
				filename, measurement);
			if (!status)
			{
				printf (" Will terminate after at least "
					"%lu ticks and %lu events.",
					min_ticks, min_events);
			}
			if ( prompt () )
				exit (EXIT_SUCCESS);
			rc = save_to_remote (server, filename, measurement,
					min_ticks, min_events, ovrwrt, async);
			exit (rc ? EXIT_FAILURE : EXIT_SUCCESS);

			break;

		case 'T':
			/* save traces to local file */
			if (strlen (measurement) || num_hist ||
					async || min_ticks || min_events || status || ovrwrt)
			{
				fprintf (stderr, "Conflicting options.\n"
					"Type %s -h for help\n", argv[0]);
				exit (EXIT_FAILURE);
			}
			if (timeout == 0)
			{
				fprintf (stderr, "You must specify a non-zero timeout.\n"
					"Type %s -h for help\n", argv[0]);
				exit (EXIT_FAILURE);
			}

			/* Proceed? */
			printf ("Will save the first average trace in the next %u seconds "
				"to local file %s.", timeout, filename);
			if ( prompt () )
				exit (EXIT_SUCCESS);
			rc = save_trace (server, filename, timeout);
			exit (rc ? EXIT_FAILURE : EXIT_SUCCESS);

			break;

		case 'H':
			/* save histograms to local file */
			if (strlen (measurement) || timeout ||
					async || min_ticks || min_events || status || ovrwrt)
			{
				fprintf (stderr, "Conflicting options.\n"
					"Type %s -h for help\n", argv[0]);
				exit (EXIT_FAILURE);
			}
			if ( ! num_hist )
				num_hist = 1;

			/* Proceed? */
			printf ("Will save %lu histograms, each of maximum size %u "
				"to local file %s.", num_hist, MAX_HIST_SIZE, filename);
			if ( prompt () )
				exit (EXIT_SUCCESS);
			rc = save_hist (server, filename, num_hist);
			exit (rc ? EXIT_FAILURE : EXIT_SUCCESS);

			break;
	}
}
