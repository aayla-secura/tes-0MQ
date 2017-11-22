#define TESPKT_DEBUG
#include "net/tespkt.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <assert.h>

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>

#ifndef NUM_LOOPS
#  define NUM_LOOPS 1 /* negative for infinite */
#endif
#ifndef SKIP
#  define SKIP 40 /* how many bytes at BOF to skip */
#endif
#ifndef NM_IFNAME
#  define NM_IFNAME "vale0:vi1"
#endif
#ifndef CAPFILE
#  define CAPFILE   "/media/data/1000_tick_cap"
#endif
#define DUMP_ROW_LEN   16 /* how many bytes per row when dumping pkt */
#define DUMP_OFF_LEN    5 /* how many digits to use for the offset */

int interrupted;

static void
dump_pkt (const unsigned char* pkt, uint32_t len)
{
	char buf[ 4*DUMP_ROW_LEN + DUMP_OFF_LEN + 2 + 1 ];

	memset (buf, 0, sizeof (buf));
	for (uint32_t r = 0; r < len; r += DUMP_ROW_LEN) {
		sprintf (buf, "%0*x: ", DUMP_OFF_LEN, r);

		/* hexdump */
		for (uint32_t b = 0; b < DUMP_ROW_LEN && b+r < len; b++)
			sprintf (buf + DUMP_OFF_LEN + 2 + 3*b, "%02x ",
				(u_int8_t)(pkt[b+r]));

		/* ASCII dump */
		for (uint32_t b = 0; b < DUMP_ROW_LEN && b+r < len; b++)
			sprintf (buf + DUMP_OFF_LEN + 2 + b + 3*DUMP_ROW_LEN,
				"%c", isprint (pkt[b+r]) ? pkt[b+r] : '.');

		printf ("%s\n", buf);
	}
	printf ("\n");
}

static void
int_hn (int sig)
{
	interrupted = 1;
}

int
main (void)
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

	/* Open the interface */
	struct nm_desc* nmd = nm_open (NM_IFNAME, NULL, 0, 0);
	if (nmd == NULL)
	{
		fprintf (stderr, "Could not open interface %s\n", NM_IFNAME);
		return -1;
	}

	/* Open the file */
	int capfd = open (CAPFILE, O_RDONLY);
	if (capfd == -1)
	{
		perror ("Cannot open file");
		return -1;
	}

	struct pollfd pfd;
	pfd.fd = nmd->fd;
	pfd.events = POLLOUT;

	int looped = 0;
	unsigned int p = 0;

	while (!interrupted && looped != NUM_LOOPS)
	{
		if (p == 0)
		{
			/* seek to BOF + SKIP */
			rc = lseek (capfd, SKIP, SEEK_SET);
			if (rc == -1)
			{
				perror ("Could not seek to BOF");
				break;
			}
			else if (rc != SKIP)
			{
				fprintf (stderr, "Could not seek to BOF\n");
				break;
			}
		}
		p++;

		tespkt pkt;
		memset (&pkt, 0, sizeof (pkt));
		/* read the header */
		rc = read (capfd, &pkt, TES_HDR_LEN);
		if (rc == -1)
		{
			perror ("Could not read in header");
			break;
		}
		else if (rc == 0)
		{ /* reached EOF */
			printf ("Reached EOF, read %u packets\n", p);

			p = 0;
			looped++;
			continue;
		}
		else if (rc != TES_HDR_LEN)
		{
			fprintf (stderr,
				"Read unexpected number of bytes "
				"from header: %d, packet no. %u\n", rc, p);
			break;
		}

		/* Read the payload */
		uint16_t len = tespkt_flen (&pkt);
		assert (len <= MAX_TES_FRAME_LEN);
		assert (len > TES_HDR_LEN);
		rc = read (capfd, (char*)&pkt + TES_HDR_LEN,
				len - TES_HDR_LEN);
		if (rc == -1)
		{
			perror ("Could not read in payload");
			break;
		}
		else if (rc != len - TES_HDR_LEN)
		{
			fprintf (stderr,
				"Read unexpected number of bytes "
				"from payload: %d, packet no. %u\n", rc, p);
			break;
		}

		/* Inject the packet */
		rc = poll (&pfd, 1, -1);
		if (rc == -1)
		{
			if (errno != EINTR)
				perror ("poll");
			break;
		}

		rc = nm_inject (nmd, &pkt, len);
		if (!rc)
		{
		        fprintf (stderr, "Cannot inject packet\n");
		        break;
		}
	}

	close (capfd);
	return 0;
}
