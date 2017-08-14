#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <czmq.h>

#define SAVEJOB_IF "tcp://localhost:55555"
#define REQ_PIC      "s81"
#define REP_PIC    "18888"

static void
usage (const char* self)
{
	fprintf (stderr,
		"Usage: %s [options]\n\n"
		"Options:\n"
		"    -f <filename>        Set the remote filename\n"
		"    -t <ticks>           Save up to that many ticks\n"
		"    -o                   Overwrite if file exists\n"
		"    -s                   Request status of filename\n"
		"\nThe 'f' option and excatly one of 's' or 't' "
		"must be specified.\n"
		"The 'o' cannot be given for status requests", self
		);
	exit (EXIT_FAILURE);
}

int
main (int argc, char **argv)
{
	char filename[128];
	char* buf = NULL;
	memset (filename, 0, sizeof (filename));
	u_int64_t max_ticks = 0;
	u_int8_t ovrwrt = 0, status = 0;

	int opt;
	while ( (opt = getopt (argc, argv, "t:f:os")) != -1 )
	{
		switch (opt)
		{
			case 't':
				if (status)
				{
					usage (argv[0]);
				}
				max_ticks = strtoul (optarg, &buf, 10);
				if (strlen (buf))
				{
					usage (argv[0]);
				}
				break;
			case 'f':
				snprintf (filename, sizeof (filename),
					"%s", optarg);
				break;
			case 'o':
				if (status)
				{
					usage (argv[0]);
				}
				ovrwrt = 1;
				break;
			case 's':
				if (max_ticks || ovrwrt)
				{
					usage (argv[0]);
				}
				max_ticks = 0;
				status = 1;
				break;
			case '?':
				usage (argv[0]);
				break;
			default:
				/* we forgot to handle an option */
				assert (0);
		}
	}
	if (strlen (filename) == 0 || argc > optind ||
		( !max_ticks && !status ))
	{
		usage (argv[0]);
	}

	printf ("Sending %s request for remote filename %s.",
		status ? "a status": (ovrwrt ? "an overwrite" : "a write" ),
		filename);
	if (!status)
	{
		printf (" Will terminate after %lu ticks.", max_ticks);
	}

	printf ("\nProceed (y/n)? ");
	do
	{
		char* line = NULL;
		size_t len = 0;
		ssize_t rlen = getline (&line, &len, stdin);

		if (line == NULL || rlen == -1)
			exit (EXIT_FAILURE);
		char rep = line[0];
		free (line);
		if (rlen == 2)
		{
			if (rep == 'n' || rep == 'N')
				exit (EXIT_SUCCESS);
			if (rep == 'y' || rep == 'Y')
				break;
		}

		printf ("Reply with 'y' or 'n': ");
	} while (1);

	zsock_t* server = zsock_new_req (">"SAVEJOB_IF);
	if (server == NULL)
	{
		fputs ("Could not connect to server", stderr);
		exit (EXIT_FAILURE);
	}
	zsock_send (server, REQ_PIC, filename, max_ticks, ovrwrt);

	puts ("Waiting for reply");

	u_int8_t fstat;
	u_int64_t ticks, size, frames, missed;
	int rc = zsock_recv (server, REP_PIC, &fstat, &ticks, &size, &frames, &missed); 
	if (rc == -1)
	{
		zsock_destroy (&server);
		exit (EXIT_FAILURE);
	}

	if (!fstat)
	{
		printf ("File %s\n", status ?
			"does not exist" : "exists");
	}
	else
	{
		printf ("%s\n"
			"ticks:         %lu\n"
			"saved frames:  %lu\n"
			"missed frames: %lu\n"
			"total size:    %lu\n",
			status ? "File contains" : "Wrote",
			ticks, frames, missed, size);
	}

	zsock_destroy (&server);
	exit (EXIT_SUCCESS);
}
