#define _WITH_GETLINE
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <assert.h>
#include <czmq.h>
#include "ansicolors.h"

#ifndef PATH_MAX
#  ifdef MAXPATHLEN
#    define PATH_MAX MAXPATHLEN
#  else
#    define PATH_MAX 4096
#  endif
#endif

static char s_prog_name[PATH_MAX];
#define OPTS_G       "Z:F:"
#define OPTS_R_ALL   "m:w:t:e:rosa"
#define OPTS_L_TRACE "w:"
#define OPTS_L_HIST  "c:"

static void
s_usage (void)
{
	fprintf (stdout,
		ANSI_BOLD "Usage: " ANSI_RESET "%s " ANSI_FG_CYAN "-Z <server> -F <filename> "
		ANSI_FG_GREEN "<command> " ANSI_FG_RED "[<command options>]" ANSI_RESET "\n\n"
		"The format for <server> is <proto>://<host>:<port>\n"
		"Command-specific options must follow command.\n"
		"Allowed commands:\n\n"
		ANSI_FG_GREEN "remote_all" ANSI_RESET ": Save frames to a remote file.\n"
		ANSI_BOLD     "  Options:\n" ANSI_RESET
		ANSI_FG_RED   "    -m <measurement>   " ANSI_RESET "Measurement name. Default is empty.\n"
		ANSI_FG_RED   "    -t <ticks>         " ANSI_RESET "Save at least that many ticks.\n"
		              "                       "            "Default is 1.\n"
		ANSI_FG_RED   "    -e <ticks>         " ANSI_RESET "Save at least that many non-tick\n"
		              "                       "            "events. Default is 0.\n"
		ANSI_FG_RED   "    -r                 " ANSI_RESET "Rename any existing measurement\n"
		              "                       "            "group of that name.\n"
		ANSI_FG_RED   "    -o                 " ANSI_RESET "Overwrite entire hdf5 file.\n"
		ANSI_FG_RED   "    -s                 " ANSI_RESET "Request status of filename.\n"
		ANSI_FG_RED   "    -a                 " ANSI_RESET "Asynchronous hdf5 conversion.\n"
		"Only one of -o and -r can be given.\n"
		"For status requests (-s) only measurement (-m) can be specified.\n\n"
		ANSI_FG_GREEN "local_trace" ANSI_RESET ": Save average traces to a local file.\n"
		ANSI_BOLD     "  Options:\n" ANSI_RESET
		ANSI_FG_RED   "    -w <timeout>       " ANSI_RESET "Timeout in seconds. Sent to the server, will\n"
		              "                       "            "receive a timeout error if no trace arrives\n"
		              "                       "            "in this period. Default is 5.\n\n"
		ANSI_FG_GREEN "local_hist" ANSI_RESET ": Save histograms to a local file.\n"
		ANSI_BOLD     "  Options:\n" ANSI_RESET
		ANSI_FG_RED   "    -c <count>         " ANSI_RESET "Save up to that many histograms.\n"
		              "                       "            "Default is 1.\n",
		s_prog_name
		);
}

static void
s_conflicting_opt (void)
{
	fprintf (stderr, "Conflicting options.\n"
		"Type %s -h for help\n", s_prog_name);
}

static void
s_invalid_arg (char opt)
{
	fprintf (stderr, "Invalid format for option %c.\n"
		"Type %s -h for help\n", opt, s_prog_name);
}

static void
s_missing_arg (char opt)
{
	fprintf (stderr, "Option %c requires an argument.\n"
		"Type %s -h for help\n", opt, s_prog_name);
}

static void
s_invalid_opt (char opt)
{
	fprintf (stderr, "Unknown option %c.\n"
		"Type %s -h for help\n", opt, s_prog_name);
}

static int
s_prompt (void)
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

/* -------------------- AVERAGE TRACE ------------------- */
#define L_TRACE_REQ_OK    0 // accepted
#define L_TRACE_REQ_INV   1 // malformed request
#define L_TRACE_REQ_TOUT  2 // timeout error
#define L_TRACE_REQ_PIC  "4"
#define L_TRACE_REP_PIC "1c"
#define L_TRACE_MAX_SIZE 65528U

static int
s_local_save_trace (const char* server, const char* filename,
	int argc, char **argv)
{
	uint32_t timeout = 5;

	/* Command-line */
	char* buf = NULL;
#ifdef GETOPT_DEBUG
	for (int a = 0; a < argc; a++)
		printf ("%s ", argv[a]);
	puts ("");
#endif
	for ( int opt = -1; (opt = getopt (argc, argv,
		":" OPTS_G OPTS_L_TRACE)) != -1; )
	{
		switch (opt)
		{
			case 'Z':
			case 'F':
				break;
			case 'w':
				timeout = strtoul (optarg, &buf, 10);
				if (strlen (buf))
				{
					s_invalid_arg (opt);
					return -1;
				}
				break;
			case '?':
				s_invalid_opt (optopt);
				return -1;
			default:
				/* we forgot to handle an option */
				assert (0);
		}
	}

	/* Proceed? */
	printf ("Will save an average trace to local file '%s'.\n"
		"Timeout is %u seconds.\n",
		filename, timeout);
	if ( s_prompt () )
		return -1;

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
	zsock_send (sock, L_TRACE_REQ_PIC, timeout);
	puts ("Waiting for reply");

	uint8_t rep;
	zchunk_t* trace;
	int rc = zsock_recv (sock, L_TRACE_REP_PIC, &rep, &trace);
	zsock_destroy (&sock);

	if (rc == -1)
		return -1;

	size_t trsize = 0;

	/* Print reply */
	printf ("\n");
	switch (rep)
	{
		case L_TRACE_REQ_INV:
			printf ("Request was not understood\n");
			break;
		case L_TRACE_REQ_TOUT:
			printf ("Request timed out\n");
			break;
		case L_TRACE_REQ_OK:
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

/* ---------------------- HISTOGRAM --------------------- */
#if 0 /* FIX */
#define L_HIST_MAX_SIZE 65528U
#else
#define L_HIST_MAX_SIZE 65576U
#endif

static int
s_local_save_hist (const char* server, const char* filename,
	int argc, char **argv)
{
	uint64_t num_hist = 1;

	/* Command-line */
	char* buf = NULL;
#ifdef GETOPT_DEBUG
	for (int a = 0; a < argc; a++)
		printf ("%s ", argv[a]);
	puts ("");
#endif
	for ( int opt = -1; (opt = getopt (argc, argv,
		":" OPTS_G OPTS_L_HIST)) != -1; )
	{
		switch (opt)
		{
			case 'Z':
			case 'F':
				break;
			case 'c':
				num_hist = strtoul (optarg, &buf, 10);
				if (strlen (buf) || num_hist == 0)
				{
					s_invalid_arg (opt);
					return -1;
				}
				break;
			case '?':
				s_invalid_opt (optopt);
				return -1;
			default:
				/* we forgot to handle an option */
				assert (0);
		}
	}
	assert (num_hist > 0);

	/* Proceed? */
	printf ("Will save %lu histogram%s to local file '%s'.\n"
		"Maximum total size is %lu.\n",
		num_hist, (num_hist > 1)? "s" : "", filename, num_hist*L_HIST_MAX_SIZE);
	if ( s_prompt () )
		return -1;

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
	int rc = posix_fallocate (fd, fsize, num_hist*L_HIST_MAX_SIZE);
	if (rc)
	{
		errno = rc; /* posix_fallocate does not set it */
		perror ("Could not allocate sufficient space");
		close (fd);
		zsock_destroy (&sock);
		return -1;
	}

	/* mmap it */
	/* TO DO: map starting at the last page boundary before end of
	 * file. */
	unsigned char* map = (unsigned char*)mmap (NULL,
		fsize + num_hist*L_HIST_MAX_SIZE, PROT_WRITE, MAP_SHARED, fd, 0);
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
	for (; ! zsys_interrupted && h < num_hist; h++)
	{
		rc = zmq_recv (sock_h, map + fsize + hsize,
			L_HIST_MAX_SIZE, 0);
		if (rc == -1)
		{
			perror ("Could not write to file");
			break;
		}
		else if ((size_t)rc > L_HIST_MAX_SIZE)
		{
			fprintf (stderr,
				"Frame is too large: %lu bytes", (size_t)rc);
			break;
		}
		hsize += (size_t)rc;
	}
	if (h < num_hist - 1)
		printf ("Saved %lu histogram%s\n",
			h, (num_hist > 1)? "s" : "");

	zsock_destroy (&sock);
	munmap (map, num_hist*L_HIST_MAX_SIZE);
	rc = ftruncate (fd, fsize + hsize);
	if (rc == -1)
		perror ("Could not truncate file");
	close (fd);
	return 0;
}

/* -------------------- SAVE-TO-FILE -------------------- */
#define R_ALL_REQ_OK    0 // accepted
#define R_ALL_REQ_INV   1 // malformed request
#define R_ALL_REQ_ABORT 2 // no such job (for status query) or file
						  // exist (for no-overwrite)
#define R_ALL_REQ_EPERM 3 // a filename is not allowed
#define R_ALL_REQ_FAIL  4 // error initializing, nothing was written
#define R_ALL_REQ_EWRT  5 // error while writing, less than minimum
						  // requested was saved
#define R_ALL_REQ_ECONV 6 // error while converting to hdf5
#define R_ALL_REQ_PIC   "ss8811"
#define R_ALL_REP_PIC "18888888"

#define R_ALL_HDF5_OVRWT_RELINK 1 /* only move existing group to
                                   * /<RG>/overwritten/<group>_<timestamp> */
#define R_ALL_HDF5_OVRWT_FILE   2 /* overwrite entire hdf5 file */

static int
s_remote_save_all (const char* server, const char* filename,
	int argc, char **argv)
{
	char measurement[1024];
	memset (measurement, 0, sizeof (measurement));
	uint64_t min_ticks = 0, min_events = 0;
	uint8_t ovrwtmode = 0, async = 0, status = 0;

	/* Command-line */
	char* buf = NULL;
#ifdef GETOPT_DEBUG
	for (int a = 0; a < argc; a++)
		printf ("%s ", argv[a]);
	puts ("");
#endif
	for ( int opt = -1; (opt = getopt (argc, argv,
		":" OPTS_G OPTS_R_ALL)) != -1; )
	{
		switch (opt)
		{
			case 'Z':
			case 'F':
				break;
			case 'm':
				snprintf (measurement, sizeof (measurement),
					"%s", optarg);
				break;
			case 't':
			case 'e':
				if (opt == 't')
					min_ticks = strtoul (optarg, &buf, 10);
				else
					min_events = strtoul (optarg, &buf, 10);

				if (strlen (buf))
				{
					s_invalid_arg (opt);
					return -1;
				}
				break;
			case 'r':
			case 'o':
				if (ovrwtmode)
				{
					s_conflicting_opt ();
					return -1;
				}

				if (opt == 'r')
					ovrwtmode = R_ALL_HDF5_OVRWT_RELINK;
				else
					ovrwtmode = R_ALL_HDF5_OVRWT_FILE;
				break;
			case 's':
				status = 1;
				break;
			case 'a':
				async = 1;
				break;
			case '?':
				s_invalid_opt (optopt);
				return -1;
			default:
				/* we forgot to handle an option */
				assert (0);
		}
	}

	/* Check for conflicting options. */
	if ( status &&
		(async || ovrwtmode || min_ticks || min_events) )
	{
		s_conflicting_opt ();
		return -1;
	}

	/* Min ticks defaults to 1. */
	if ( ! min_ticks && ! status )
		min_ticks = 1;

	/* Proceed? */
	if (status)
	{
		printf ("Sending a status request for remote filename "
			"'%s' and measurement group '%s'.\n",
			filename, measurement);
	}
	else
	{
		printf ("Sending a%s request for remote filename "
			"'%s' and measurement group '%s'.\n"
			"%sWill terminate after at least "
			"%lu ticks and %lu events.\n",
			async ? "n asynchronous" : "",
			filename, measurement,
			(ovrwtmode == R_ALL_HDF5_OVRWT_FILE) ?
				"Will overwrite file.\n" : 
				(ovrwtmode == R_ALL_HDF5_OVRWT_RELINK) ?
					"Will backup measurement group.\n" : "",
			min_ticks, min_events);
	}
	if ( s_prompt () )
		return -1;

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
	zsock_send (sock, R_ALL_REQ_PIC,
		filename,
		measurement,
		min_ticks,
		min_events,
		ovrwtmode,
		async);
	puts ("Waiting for reply");

	uint8_t fstat;
	uint64_t ticks, events, traces, hists, frames, missed, dropped;
	int rc = zsock_recv (sock, R_ALL_REP_PIC,
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
		case R_ALL_REQ_INV:
			printf ("Request was not understood\n");
			break;
		case R_ALL_REQ_ABORT:
			printf ("File %s\n", min_ticks ?
				"exists" : "does not exist");
			break;
		case R_ALL_REQ_EPERM:
			printf ("Filename is not allowed\n");
			break;
		case R_ALL_REQ_FAIL:
			printf ("Unknown error while initializing\n\n");
			break;
		case R_ALL_REQ_EWRT:
			printf ("Unknown error while writing\n\n");
			/* fallthrough */
		case R_ALL_REQ_ECONV:
			printf ("Unknown error while converting\n\n");
			/* fallthrough */
		case R_ALL_REQ_OK:
			printf ("%s\n"
				"ticks:          %lu\n"
				"other events:   %lu\n"
				"traces:         %lu\n"
				"histograms:     %lu\n"
				"saved frames:   %lu\n"
				"missed frames:  %lu\n"
				"dropped frames: %lu\n",
				status ? "Wrote" : "File contains",
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
	strncpy (s_prog_name, argv[0], sizeof (s_prog_name));
	zsys_init ();
	zsys_catch_interrupts ();

	/* Command-line */
	char server[1024];
	memset (server, 0, sizeof (server));
	char filename[PATH_MAX];
	memset (filename, 0, sizeof (filename));

	/* Handle missing arguments, but not unknown options here. */
#ifdef GETOPT_DEBUG
	for (int a = 0; a < argc; a++)
		printf ("%s ", argv[a]);
	puts ("");
#endif
	for ( int opt = -1; (opt = getopt (argc, argv,
		":h" OPTS_G OPTS_R_ALL OPTS_L_TRACE OPTS_L_HIST)) != -1; )
	{
		switch (opt)
		{
			case 'Z':
				snprintf (server, sizeof (server),
					"%s", optarg);
				break;
			case 'F':
				snprintf (filename, sizeof (filename),
					"%s", optarg);
				break;
			case 'h':
				s_usage ();
				exit (EXIT_SUCCESS);
			case ':': /* missing argument to option */
				s_missing_arg (optopt);
				exit (EXIT_FAILURE);
		}
	}

	/* getopt won't complain from empty string argument. */
	if (strlen (server) == 0)
	{
		fprintf (stderr, "You must specify the remote address.\n"
			"Type %s -h for help\n", s_prog_name);
		exit (EXIT_FAILURE);
	}
	if (strlen (filename) == 0)
	{
		fprintf (stderr, "You must specify a filename.\n"
			"Type %s -h for help\n", s_prog_name);
		exit (EXIT_FAILURE);
	}

	/* Get command argument. */
	if (optind == argc)
	{
		fprintf (stderr, "Missing command\n");
		exit (EXIT_FAILURE);
	}
	else if (optind + 1 > argc)
	{
		fprintf (stderr, "Extra arguments\n");
		exit (EXIT_FAILURE);
	}
	char* cmd = argv[optind];
	optind = 1; /* reset getopt position */
	int rc = 0;
	if (strcmp (cmd, "remote_all") == 0)
		rc = s_remote_save_all (server, filename, argc, argv);
	else if (strcmp (cmd, "local_trace") == 0)
		rc = s_local_save_trace (server, filename, argc, argv);
	else if (strcmp (cmd, "local_hist") == 0)
		rc = s_local_save_hist (server, filename, argc, argv);
	else
	{
		printf ("Unknown command %s\n", cmd);
		rc = -1;
	}
	exit (rc ? EXIT_FAILURE : EXIT_SUCCESS);
}
