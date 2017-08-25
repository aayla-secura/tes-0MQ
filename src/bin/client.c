#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <czmq.h>

#define REQ_PIC        "s81"
#define REP_PIC      "18888"
#define MAX_HISTSIZE 65528

int interrupted;

static void
usage (const char* self)
{
	fprintf (stdout,
		"Usage: %s -R <socket> [options]\n\n"
		"The format for <socket> is <proto>://<host>:<port>\n\n"
		"The client operates in one of two modes:\n"
		"1) Options for saving all frames to a remote file:\n"
		"    -f <filename>      Remote filename\n"
		"    -t <ticks>         Save up to that many ticks\n"
		"    -o                 Overwrite if file exists\n"
		"    -s                 Request status of filename\n"
		"The 'f' option and exactly one of 's' or 't' "
		"must be specified.\n"
		"The 'o' cannot be given for status requests\n\n"
		"2) Options for saving histograms to a local file:\n"
		"    -f <filename>      Local filename. Will append if"
		"                       existing.\n"
		"    -c <count>         Save up to that many histograms\n"
		"Both 'f' and 'c' options must be given.\n", self
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
		uint64_t max_ticks, uint8_t ovrwrt)
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
	zsock_send (sock, REQ_PIC, filename, max_ticks, ovrwrt);
	puts ("Waiting for reply");

	uint8_t fstat;
	uint64_t ticks, size, frames, missed;
	int rc = zsock_recv (sock, REP_PIC, &fstat, &ticks, &size, &frames, &missed); 
	if (rc == -1)
	{
		zsock_destroy (&sock);
		return -1;
	}

	/* Print reply */
	if (!fstat)
	{
		printf ("File %s\n", max_ticks ?
			"exists" : "does not exist");
	}
	else
	{
		printf ("%s\n"
			"ticks:         %lu\n"
			"saved frames:  %lu\n"
			"missed frames: %lu\n"
			"total size:    %lu\n",
			max_ticks ? "Wrote" : "File contains",
			ticks, frames, missed, size);
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
		return -1;
	}

	/* Command-line */
	char server[256];
	memset (server, 0, sizeof (server));
	char filename[256];
	memset (filename, 0, sizeof (filename));
	char* buf = NULL;
	uint64_t max_ticks = 0, numhist = 0;
	uint8_t ovrwrt = 0, status = 0;
	/* 0 for remotely save all frames, 1 for locally save histograms */
	int mode = -1;

	int opt;
	while ( (opt = getopt (argc, argv, "R:f:c:t:osh")) != -1 )
	{
		switch (opt)
		{
			case 'R':
				snprintf (server, sizeof (server),
						"%s", optarg);
				break;
			case 'c':
				if (mode == 0)
				{
					fprintf (stderr, "Option %c is not "
						"valid in mode %d.\n", opt, mode + 1);
					// usage (argv[0]);
				}
				mode = 1;
				numhist = strtoul (optarg, &buf, 10);
				if (strlen (buf))
				{
					fprintf (stderr, "Invalid format for "
						"option %c.\n", opt);
					// usage (argv[0]);
				}
				break;
			case 't':
				if (mode == 1)
				{
					fprintf (stderr, "Option %c is not "
						"valid in mode %d.\n", opt, mode + 1);
					// usage (argv[0]);
				}
				mode = 0;
				if (status)
				{
					fprintf (stderr, "Option %c is not "
						"valid for status requests.\n", opt);
					// usage (argv[0]);
				}
				max_ticks = strtoul (optarg, &buf, 10);
				if (strlen (buf))
				{
					fprintf (stderr, "Invalid format for "
						"option %c.\n", opt);
					// usage (argv[0]);
				}
				break;
			case 'f':
				snprintf (filename, sizeof (filename),
						"%s", optarg);
				break;
			case 'o':
				if (mode == 1)
				{
					fprintf (stderr, "Option %c is not "
						"valid in mode %d.\n", opt, mode + 1);
					// usage (argv[0]);
				}
				mode = 0;
				if (status)
				{
					fprintf (stderr, "Option %c is not "
						"valid for status requests.\n", opt);
					// usage (argv[0]);
				}
				ovrwrt = 1;
				break;
			case 's':
				if (mode == 1)
				{
					fprintf (stderr, "Option %c is not "
						"valid in mode %d.\n", opt, mode + 1);
					// usage (argv[0]);
				}
				mode = 0;
				if (max_ticks || ovrwrt)
				{
					fprintf (stderr, "Option %c is not "
						"valid for status requests.\n", opt);
					// usage (argv[0]);
				}
				max_ticks = 0;
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
	/* Handle missing mandatory options */
	if (strlen (server) == 0)
	{
		fprintf (stderr, "You must specify the remote address.\n"
			"Type %s -h for help\n", argv[0]);
	}
	if (strlen (filename) == 0)
	{
		fprintf (stderr, "You must specify a filename.\n"
			"Type %s -h for help\n", argv[0]);
	}
	if (argc > optind)
	{
		fprintf (stderr, "Extra arguments given.\n"
			"Type %s -h for help\n", argv[0]);
	}
	if ( mode == 0 && !max_ticks && !status )
	{
		fprintf (stderr, "Excatly one of 's' or 't' options "
			"must be specified.\n"
			"Type %s -h for help\n", argv[0]);
	}
	if (mode == 1 && numhist == 0)
	{
		fprintf (stderr, "You must specify a positive number of histograms. "
			"Type %s -h for help\n", argv[0]);
	}
	if (mode == -1)
	{
		fprintf (stderr, "You must choose a mode of operation by "
			"giving at least one of its specific options.\n"
			"Type %s -h for help\n", argv[0]);
	}

	/* Prompt and take action */
	if (mode == 0)
	{
		printf ("Sending %s request for remote filename %s.",
			status ? "a status": (ovrwrt ? "an overwrite" : "a write" ),
			filename);
		if (!status)
		{
			printf (" Will terminate after %lu ticks.", max_ticks);
		}
		if ( prompt () )
			return 0;
		return save_to_remote (server, filename, max_ticks, ovrwrt);
	}
	else
	{
		printf ("Will save %lu histograms, each of maximum size %u "
			"to local file %s.", numhist, MAX_HISTSIZE, filename);
		if ( prompt () )
			return 0;
		return save_hist (server, filename, numhist);
	}
}
