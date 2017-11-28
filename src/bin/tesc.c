#define _WITH_GETLINE
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <czmq.h>

#define REQ_OK    0 // accepted
#define REQ_INV   1 // malformed request
#define REQ_ABORT 2 // no such job (for status query) or file exist (for no-overwrite)
#define REQ_EPERM 3 // filename is not allowed
#define REQ_FAIL  4 // other error opening the file, nothing was written
#define REQ_ERR   5 // error while writing, less than minimum requested was saved
#define REQ_PIC    "s881"
#define REP_PIC "1888888"
#if 0 /* FIX */
#define MAX_HISTSIZE 65528
#else
#define MAX_HISTSIZE 65576
#endif

int interrupted;

static void
usage (const char* self)
{
	fprintf (stdout,
		"Usage: %s -Z <server> [options]\n"
		"The format for <server> is <proto>://<host>:<port>\n\n"
		"The client operates in one of two modes:\n"
		"Save streams to a remote file: Selected by the 'R' option.\n"
		"  Options:\n"
		"    -R <filename>      Remote filename.\n"
		"    -t <ticks>         Save at least that many ticks.\n"
		"                       Default is 1.\n"
		"    -e <ticks>         Save at least that many non-tick\n"
	       	"                       events. Default is 0.\n"
		"    -o                 Overwrite if file exists.\n"
		"    -s                 Request status of filename.\n"
		"The 'o', 't' or 'e' options cannot be given for status\n"
	        "                       requests.\n\n"
		"Save histograms to a local file: Selected by the 'L' option.\n"
		"  Options:\n"
		"    -L <filename>      Local filename. Will append if\n"
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
	printf ("Appending to file of size %lu\n", fsize);

	/* Allocate space */
	int rc = posix_fallocate (fd, fsize, cnt*MAX_HISTSIZE);
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
		fsize + cnt*MAX_HISTSIZE, PROT_WRITE, MAP_SHARED, fd, 0);
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
		rc = zmq_recv (sock_h, map + fsize + hsize, MAX_HISTSIZE, 0);
		if (rc == -1)
		{
			perror ("Could not write to file");
			break;
		}
		else if ((size_t)rc > MAX_HISTSIZE)
		{
			fprintf (stderr, "Frame is too large: %lu bytes", (size_t)rc);
			break;
		}
		hsize += (size_t)rc;
	}
	if (h < cnt - 1)
		printf ("Saved %lu histograms\n", h);

	zsock_destroy (&sock);
	munmap (map, cnt*MAX_HISTSIZE);
	rc = ftruncate (fd, fsize + hsize);
	if (rc == -1)
		perror ("Could not truncate file");
	close (fd);
	return 0;
}

static int
save_to_remote (const char* server, const char* filename,
		uint64_t min_ticks, uint64_t min_events, uint8_t ovrwrt)
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
	zsock_send (sock, REQ_PIC, filename, min_ticks, min_events, ovrwrt);
	puts ("Waiting for reply");

	uint8_t fstat;
	uint64_t ticks, events, traces, hists, frames, missed;
	int rc = zsock_recv (sock, REP_PIC,
			&fstat,
			&ticks,
			&events,
			&traces,
			&hists,
			&frames,
			&missed); 
	if (rc == -1)
	{
		zsock_destroy (&sock);
		return -1;
	}

	/* Print reply */
	printf ("\n");
	switch (fstat)
	{
		case REQ_INV:
			printf ("Request was not understood\n");
			break;
		case REQ_ABORT:
			printf ("File %s\n", min_ticks ?
				"exists" : "does not exist");
			break;
		case REQ_EPERM:
			printf ("Filename is not allowed\n");
			break;
		case REQ_FAIL:
			printf ("Unknown error while opening\n\n");
			break;
		case REQ_ERR:
			printf ("Unknown error while writing\n\n");
			/* fallthrough */
		case REQ_OK:
			printf ("%s\n"
				"ticks:         %lu\n"
				"other events:  %lu\n"
				"traces:        %lu\n"
				"histograms:    %lu\n"
				"saved frames:  %lu\n"
				"missed frames: %lu\n",
				min_ticks ? "Wrote" : "File contains",
				ticks, events, traces, hists, frames, missed);
			break;
		default:
			assert (0);
	}

	zsock_destroy (&sock);
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
	char server[256];
	memset (server, 0, sizeof (server));
	char filename[256];
	memset (filename, 0, sizeof (filename));
	char* buf = NULL;
	uint64_t min_ticks = 0, min_events = 0, num_hist = 0;
	uint8_t ovrwrt = 0, status = 0;
	/* 0 for remotely save all frames, 1 for locally save histograms */
	int mode = -1;

	int opt;
	while ( (opt = getopt (argc, argv, "Z:L:R:c:t:e:osh")) != -1 )
	{
		switch (opt)
		{
			case 'Z':
				snprintf (server, sizeof (server),
						"%s", optarg);
				break;
			case 'R':
			case 'L':
				snprintf (filename, sizeof (filename),
						"%s", optarg);
				mode = ( (opt == 'R') ? 0 : 1 );
				break;
			case 'c':
				num_hist = strtoul (optarg, &buf, 10);
				if (strlen (buf))
				{
					fprintf (stderr, "Invalid format for "
						"option %c.\n", opt);
					// usage (argv[0]);
					exit (EXIT_FAILURE);
				}
				break;
			case 't':
				min_ticks = strtoul (optarg, &buf, 10);
				if (strlen (buf))
				{
					fprintf (stderr, "Invalid format for "
						"option %c.\n", opt);
					// usage (argv[0]);
					exit (EXIT_FAILURE);
				}
				break;
			case 'e':
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
	assert (mode != -1);

	/* Handle conflicting options and set defaults. */
	if (mode == 0)
	{ /* save frames to remote file */
		if ( num_hist ||
			( status && (ovrwrt || min_ticks || min_events) ) )
		{
			fprintf (stderr, "Conflicting options.\n"
				"Type %s -h for help\n", argv[0]);
			exit (EXIT_FAILURE);
		}
		if ( ! min_ticks && ! status )
			min_ticks = 1;
	}
	else
	{ /* save histograms to local file */
		if (min_ticks || min_events || status || ovrwrt)
		{
			fprintf (stderr, "Conflicting options.\n"
				"Type %s -h for help\n", argv[0]);
			exit (EXIT_FAILURE);
		}
		if ( ! num_hist )
			num_hist = 1;
	}

	/* Prompt and take action */
	if (mode == 0)
	{
		printf ("Sending %s request for remote filename %s.",
			status ? "a status": (ovrwrt ? "an overwrite" : "a write" ),
			filename);
		if (!status)
		{
			printf (" Will terminate after at least %lu ticks and %lu events.",
				min_ticks, min_events);
		}
		if ( prompt () )
			exit (EXIT_SUCCESS);
		int rc = save_to_remote (server, filename, min_ticks, min_events, ovrwrt);
		exit (rc ? EXIT_FAILURE : EXIT_SUCCESS);
	}
	else
	{
		printf ("Will save %lu histograms, each of maximum size %u "
			"to local file %s.", num_hist, MAX_HISTSIZE, filename);
		if ( prompt () )
			exit (EXIT_SUCCESS);
		int rc = save_hist (server, filename, num_hist);
		exit (rc ? EXIT_FAILURE : EXIT_SUCCESS);
	}
}
