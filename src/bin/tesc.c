#define _WITH_GETLINE
#ifdef linux
#  define _GNU_SOURCE /* strchrnul */
#endif
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <assert.h>
#include <czmq.h>
#include "api.h"
#include "ansicolors.h"

#ifndef PATH_MAX
#  ifdef MAXPATHLEN
#    define PATH_MAX MAXPATHLEN
#  else
#    define PATH_MAX 4096
#  endif
#endif

typedef int (cmd_hn)(const char*, const char*, int, char**);

static char s_prog_name[PATH_MAX];
#define OPTS_G       "Z:F:" /* processed by main */
/* The options accepting arguments should be consistent across the
 * different subcommands, i.e. if one command has 'c' with no argument,
 * another one cannot have 'c:' with an argument */
#define OPTS_S_INFO  "w:"
#define OPTS_J_CONF  "t:R:"
#define OPTS_R_ALL   "m:w:t:e:rocCa"
#define OPTS_L_TRACE "w:"
#define OPTS_L_HIST  "n:" /* both jitter and mca */

static void
s_usage (void)
{
	fprintf (stdout,
		ANSI_BOLD "Usage: " ANSI_RESET "%s " ANSI_FG_CYAN "-Z <server>"
		ANSI_FG_GREEN "<command> " ANSI_FG_RED "[<command options>]" ANSI_RESET "\n\n"
		"The format for <server> is <proto>://<host>[:<port>]\n"
		"Port defaults to the default port for the selected task.\n"
		"Allowed commands:\n\n"
		ANSI_FG_GREEN "server_info" ANSI_RESET ": Gets packet rate statistics.\n"
		ANSI_BOLD     "  Options:\n" ANSI_RESET
		ANSI_FG_RED   "    -w <seconds>       " ANSI_RESET "Number of seconds to accumulate for.\n"
		              "                                     Default is 1.\n\n"
		ANSI_FG_GREEN "jitter_conf" ANSI_RESET ": Configure or query jitter histogram configuration.\n"
		ANSI_BOLD     "  Options:\n" ANSI_RESET
		ANSI_FG_RED   "    -t <ticks>         " ANSI_RESET "Number of ticks to accumulate for.\n"
		              "                                     Default is 0 (query setting).\n\n"
		ANSI_FG_RED   "    -R <channel>       " ANSI_RESET "Event channel to trigger on.\n"
		              "                                     Default is 0.\n\n"
		ANSI_FG_GREEN "remote_all" ANSI_RESET ": Save frames to a remote file.\n"
		ANSI_BOLD     "  Options:\n" ANSI_RESET
		ANSI_FG_RED   "    -F <filename>      " ANSI_RESET "Remote filename.\n"
		ANSI_FG_RED   "    -m <measurement>   " ANSI_RESET "Measurement name. Default is empty.\n"
		ANSI_FG_RED   "    -t <ticks>         " ANSI_RESET "Save at least that many ticks.\n"
		              "                       "            "Default is 0.\n"
		ANSI_FG_RED   "    -e <evens>         " ANSI_RESET "Save at least that many non-tick\n"
		              "                       "            "events. Default is 0.\n"
		ANSI_FG_RED   "    -r                 " ANSI_RESET "Rename any existing measurement\n"
		              "                       "            "group of that name.\n"
		ANSI_FG_RED   "    -o                 " ANSI_RESET "Overwrite entire hdf5 file.\n"
		ANSI_FG_RED   "    -c                 " ANSI_RESET "Capture only, no conversion.\n"
		ANSI_FG_RED   "    -C                 " ANSI_RESET "Convert only, no capture.\n"
		ANSI_FG_RED   "    -a                 " ANSI_RESET "Asynchronous hdf5 conversion.\n"
		"Only one of -o and -r can be given.\n"
		"For status requests (-s) only measurement (-m) can be specified.\n\n"
		ANSI_FG_GREEN "local_trace" ANSI_RESET ": Save average traces to a local file.\n"
		ANSI_BOLD     "  Options:\n" ANSI_RESET
		ANSI_FG_RED   "    -F <filename>      " ANSI_RESET "Local filename.\n"
		ANSI_FG_RED   "    -w <timeout>       " ANSI_RESET "Timeout in seconds. Sent to the server, will\n"
		              "                       "            "receive a timeout error if no trace arrives\n"
		              "                       "            "in this period. Default is 5.\n\n"
		ANSI_FG_GREEN "local_mca" ANSI_RESET ": Save histograms to a local file.\n"
		ANSI_BOLD     "  Options:\n" ANSI_RESET
		ANSI_FG_RED   "    -F <filename>      " ANSI_RESET "Local filename.\n"
		ANSI_FG_RED   "    -n <count>         " ANSI_RESET "Save up to that many histograms.\n"
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

/* --------------------- PACKET INFO -------------------- */

static int
s_server_info (const char* server, const char* filename,
	int argc, char** argv)
{
	uint32_t timeout = 1;

	/* Command-line */
	char* buf = NULL;
#ifdef GETOPT_DEBUG
	for (int a = 0; a < argc; a++)
		printf ("%s ", argv[a]);
	puts ("");
#endif
	while (optind < argc)
	{
		int opt = getopt (argc, argv, "+:" OPTS_G OPTS_S_INFO);
		if (opt == -1)
		{
			optind++;
			continue;
		}
		switch (opt)
		{
			case 'Z':
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
			case ':': /* missing argument to option */
				/* this should have been caught in main */
				assert (0);
			default:
				/* we forgot to handle an option */
				assert (0);
		}
	}

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
	zsock_send (sock, TES_INFO_REQ_PIC, timeout);
	puts ("Waiting for reply");

	uint8_t rep;
	uint64_t processed, missed, bad, ticks, mcas, traces, events;
	int rc = zsock_recv (sock, TES_INFO_REP_PIC,
		&rep,
		&processed,
		&missed,
		&bad,
		&ticks,
		&mcas,
		&traces,
		&events); 
	zsock_destroy (&sock);

	if (rc == -1)
		return -1;

	/* Print reply */
	printf ("\n");
	switch (rep)
	{
		case TES_INFO_REQ_EINV:
			printf ("Request was not understood\n");
			break;
		case TES_INFO_REQ_OK:
			printf (
				"processed packets: %lu\n"
				"missed packets:    %lu\n"
				"bad packets:       %lu\n"
				"ticks:             %lu\n"
				"mcas:              %lu\n"
				"traces:            %lu\n"
				"other events:      %lu\n",
				processed,
				missed,
				bad,
				ticks,
				mcas,
				traces,
				events);
			break;
		default:
			assert (0);
	}

	return 0;
}

/* --------------------- JITTER CONF -------------------- */

static int
s_jitter_conf (const char* server, const char* filename,
	int argc, char** argv)
{
	uint64_t ticks = 0;
	uint8_t ref_ch = 0;

	/* Command-line */
	char* buf = NULL;
#ifdef GETOPT_DEBUG
	for (int a = 0; a < argc; a++)
		printf ("%s ", argv[a]);
	puts ("");
#endif
	while (optind < argc)
	{
		int opt = getopt (argc, argv, "+:" OPTS_G OPTS_J_CONF);
		if (opt == -1)
		{
			optind++;
			continue;
		}
		switch (opt)
		{
			case 'Z':
			case 'F':
				break;
			case 't':
			case 'R':
				if (opt == 't')
					ticks = strtoul (optarg, &buf, 10);
				else
					ref_ch = strtoul (optarg, &buf, 10);

				if (strlen (buf))
				{
					s_invalid_arg (opt);
					return -1;
				}
				break;
			case '?':
				s_invalid_opt (optopt);
				return -1;
			case ':': /* missing argument to option */
				/* this should have been caught in main */
				assert (0);
			default:
				printf ("%c, %c, %d\n",opt,optopt, optind);
				/* we forgot to handle an option */
				assert (0);
		}
	}

	/* Proceed? */
	if (ticks > 0)
	{
		printf ("Configuring jitter to accumulate over %lu ticks"
				"and trigger on channel %hhu\n", ticks, ref_ch);
		if ( s_prompt () )
			return -1;
	}

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
	zsock_send (sock, TES_JITTER_REQ_PIC, ref_ch, ticks);
	puts ("Waiting for reply");

	int rc = zsock_recv (sock, TES_CAP_REP_PIC, &ref_ch, &ticks);
	zsock_destroy (&sock);

	if (rc == -1)
		return -1;

	/* Print reply */
	printf ("\n");
	printf ("Set values are: ticks = %lu, ref channel = %hhu\n",
		ticks, ref_ch);

	return 0;
}

/* -------------------- AVERAGE TRACE ------------------- */

static int
s_local_save_trace (const char* server, const char* filename,
	int argc, char** argv)
{
	uint32_t timeout = 5;

	/* Command-line */
	char* buf = NULL;
#ifdef GETOPT_DEBUG
	for (int a = 0; a < argc; a++)
		printf ("%s ", argv[a]);
	puts ("");
#endif
	while (optind < argc)
	{
		int opt = getopt (argc, argv, "+:" OPTS_G OPTS_L_TRACE);
		if (opt == -1)
		{
			optind++;
			continue;
		}
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
			case ':': /* missing argument to option */
				/* this should have been caught in main */
				assert (0);
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
	zsock_send (sock, TES_AVGTR_REQ_PIC, timeout);
	puts ("Waiting for reply");

	uint8_t rep;
	zchunk_t* trace;
	int rc = zsock_recv (sock, TES_AVGTR_REP_PIC, &rep, &trace);
	zsock_destroy (&sock);

	if (rc == -1)
		return -1;

	size_t trsize = 0;

	/* Print reply */
	printf ("\n");
	switch (rep)
	{
		case TES_AVGTR_REQ_EINV:
			printf ("Request was not understood\n");
			break;
		case TES_AVGTR_REQ_ETOUT:
			printf ("Request timed out\n");
			break;
		case TES_AVGTR_REQ_OK:
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

static int
s_local_save_hist (const char* server, const char* filename,
	int argc, char** argv, size_t max_size)
{
	uint64_t num_hist = 1;

	/* Command-line */
	char* buf = NULL;
#ifdef GETOPT_DEBUG
	for (int a = 0; a < argc; a++)
		printf ("%s ", argv[a]);
	puts ("");
#endif
	while (optind < argc)
	{
		int opt = getopt (argc, argv, "+:" OPTS_G OPTS_L_HIST);
		if (opt == -1)
		{
			optind++;
			continue;
		}
		switch (opt)
		{
			case 'Z':
			case 'F':
				break;
			case 'n':
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
			case ':': /* missing argument to option */
				/* this should have been caught in main */
				assert (0);
			default:
				/* we forgot to handle an option */
				assert (0);
		}
	}
	assert (num_hist > 0);

	/* Proceed? */
	printf ("Will save %lu histogram%s to local file '%s'.\n"
		"Maximum total size is %lu.\n",
		num_hist, (num_hist > 1)? "s" : "", filename, num_hist*max_size);
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
	int rc = posix_fallocate (fd, fsize, num_hist*max_size);
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
		fsize + num_hist*max_size,
		PROT_WRITE, MAP_SHARED, fd, 0);
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
		rc = zmq_recv (sock_h, map + fsize + hsize, max_size, 0);
		if (rc == -1)
		{
			perror ("Could not write to file");
			break;
		}
		else if ((size_t)rc > max_size)
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
	munmap (map, num_hist*max_size);
	rc = ftruncate (fd, fsize + hsize);
	if (rc == -1)
		perror ("Could not truncate file");
	close (fd);
	return 0;
}

/* -------------------- MCA HISTOGRAM ------------------- */

static int
s_local_save_mca (const char* server, const char* filename,
	int argc, char** argv)
{
	return s_local_save_hist (server, filename,
		argc, argv, TES_HIST_MAXSIZE);
}

/* ------------------ JITTER HISTOGRAM ------------------ */

static int
s_local_save_jitter (const char* server, const char* filename,
	int argc, char** argv)
{
	return s_local_save_hist (server, filename,
		argc, argv, TES_JITTER_SIZE);
}

/* ------------------- REMOTE CAPTURE ------------------- */

static int
s_remote_save_all (const char* server, const char* filename,
	int argc, char** argv)
{
	char measurement[1024] = {0};
	uint64_t min_ticks = 0, min_events = 0;
	uint8_t ovrwtmode = 0, async = 0, capmode = 0;

	/* Command-line */
	char* buf = NULL;
#ifdef GETOPT_DEBUG
	for (int a = 0; a < argc; a++)
		printf ("%s ", argv[a]);
	puts ("");
#endif
	while (optind < argc)
	{
		int opt = getopt (argc, argv, "+:" OPTS_G OPTS_R_ALL);
		if (opt == -1)
		{
			optind++;
			continue;
		}
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
					ovrwtmode = TES_H5_OVRWT_RELINK;
				else
					ovrwtmode = TES_H5_OVRWT_FILE;
				break;
			case 'c':
			case 'C':
				if (capmode)
				{
					s_conflicting_opt ();
					return -1;
				}

				if (opt == 'c')
					capmode = TES_CAP_CAPONLY;
				else
					capmode = TES_CAP_CONVONLY;
				break;
			case 'a':
				async = 1;
				break;
			case '?':
				s_invalid_opt (optopt);
				return -1;
			case ':': /* missing argument to option */
				/* this should have been caught in main */
				assert (0);
			default:
				printf ("%c, %c, %d\n",opt,optopt, optind);
				/* we forgot to handle an option */
				assert (0);
		}
	}

	/* Proceed? */
	if (capmode == TES_CAP_AUTO && ! min_ticks && ! min_events)
	{
		printf ("Sending a status request for remote filename "
			"'%s' and measurement group '%s'.\n",
			filename, measurement);
	}
	else
	{
		printf ("Sending a%s %s request for remote filename "
			"'%s' and measurement group '%s'.\n"
			"%sWill terminate after at least "
			"%lu ticks and %lu events.\n",
			async ? "n asynchronous" : "",
			capmode == TES_CAP_CONVONLY ? "conversion only" :
				(capmode == TES_CAP_CAPONLY ? "capture only" :
				"capture"),
			filename, measurement,
			(ovrwtmode == TES_H5_OVRWT_FILE) ?
				"Will overwrite file.\n" : 
				(ovrwtmode == TES_H5_OVRWT_RELINK) ?
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
	zsock_send (sock, TES_CAP_REQ_PIC,
		filename,
		measurement,
		min_ticks,
		min_events,
		ovrwtmode,
		async,
		capmode);
	puts ("Waiting for reply");

	uint8_t fstat;
	uint64_t ticks, events, traces, hists, frames, missed, dropped;
	int rc = zsock_recv (sock, TES_CAP_REP_PIC,
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
		case TES_CAP_REQ_EINV:
			printf ("Request was not understood\n");
			break;
		case TES_CAP_REQ_EABORT:
			printf ("File %s\n", min_ticks ?
				"exists" : "does not exist");
			break;
		case TES_CAP_REQ_EPERM:
			printf ("Filename is not allowed\n");
			break;
		case TES_CAP_REQ_EFAIL:
			printf ("Unknown error while initializing\n\n");
			break;
		case TES_CAP_REQ_EWRT:
			printf ("Unknown error while writing\n\n");
			/* fallthrough */
		case TES_CAP_REQ_ECONV:
			printf ("Unknown error while converting\n\n");
			/* fallthrough */
		case TES_CAP_REQ_EFIN:
			printf ("Unknown error while finalizing\n\n");
			/* fallthrough */
		case TES_CAP_REQ_OK:
			printf ("%s\n"
				"ticks:          %lu\n"
				"other events:   %lu\n"
				"traces:         %lu\n"
				"histograms:     %lu\n"
				"saved frames:   %lu\n"
				"missed frames:  %lu\n"
				"dropped frames: %lu\n",
				(min_ticks || min_events) ? "Wrote" : "File contains",
				ticks, events, traces, hists, frames, missed, dropped);
			break;
		default:
			assert (0);
	}

	return 0;
}

int
main (int argc, char** argv)
{
	strncpy (s_prog_name, argv[0], sizeof (s_prog_name));
	zsys_init ();
	zsys_catch_interrupts ();

	/* Command-line */
	char server[1024] = {0};
	char filename[PATH_MAX] = {0};
	char cmd[64] = {0};

#ifdef GETOPT_DEBUG
	for (int a = 0; a < argc; a++)
		printf ("%s ", argv[a]);
	puts ("");
#endif
	while (optind < argc)
	{
#ifdef GETOPT_DEBUG
			printf ("Processing option at index %d\n", optind);
#endif
		int opt = getopt (argc, argv,
			"+:h" OPTS_G OPTS_S_INFO OPTS_J_CONF OPTS_L_TRACE OPTS_L_HIST OPTS_R_ALL);
		if (opt == -1)
		{
#ifdef GETOPT_DEBUG
			printf ("Stopped processing options at index %d\n", optind);
#endif
			/* We understand only one argument at the moment, cmd */
			if (strlen (cmd) != 0)
			{
				fprintf (stderr, "Extra arguments\n");
				exit (EXIT_FAILURE);
			}
			strncpy (cmd, argv[optind], sizeof (cmd));
			optind++;
			continue;
		}
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
			case ':':
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

	/* Get command argument. */
	if (strlen (cmd) == 0)
	{
		fprintf (stderr, "Missing command\n");
		exit (EXIT_FAILURE);
	}
	optind = 1; /* reset getopt position */
	cmd_hn* callback = NULL;
	char* defport = NULL;
	bool require_filename = 1;

	if (strcmp (cmd, "server_info") == 0)
	{
		callback = s_server_info;
		defport = TES_INFO_LPORT;
		require_filename = 0;
	}
	else if (strcmp (cmd, "jitter_conf") == 0)
	{
		callback = s_jitter_conf;
		defport = TES_JITTER_REP_LPORT;
		require_filename = 0;
	}
	else if (strcmp (cmd, "remote_all") == 0)
	{
		callback = s_remote_save_all;
		defport = TES_CAP_LPORT;
	}
	else if (strcmp (cmd, "local_trace") == 0)
	{
		callback = s_local_save_trace;
		defport = TES_AVGTR_LPORT;
	}
	else if (strcmp (cmd, "local_mca") == 0)
	{
		callback = s_local_save_mca;
		defport = TES_HIST_LPORT;
	}
	else if (strcmp (cmd, "local_jitter") == 0)
	{
		callback = s_local_save_jitter;
		defport = TES_JITTER_PUB_LPORT;
	}
	else
	{
		printf ("Unknown command %s\n", cmd);
		exit (EXIT_FAILURE);
	}
	assert (defport != NULL);
	assert (callback != NULL);

	/* Did user supply port? */
	if (strchr (strchrnul (server, '/'), ':') == NULL)
	{
		printf ("Port defaults to %s\n", defport);
		snprintf (server + strlen (server),
			sizeof (server) - strlen (server), ":%s", defport);
	}

	if (require_filename && (strlen (filename) == 0))
	{
		fprintf (stderr, "You must specify a filename.\n"
				"Type %s -h for help\n", s_prog_name);
		exit (EXIT_FAILURE);
	}

	int rc = callback (server, filename, argc, argv);
	exit (rc ? EXIT_FAILURE : EXIT_SUCCESS);
}
