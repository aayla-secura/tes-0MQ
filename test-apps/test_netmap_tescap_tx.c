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
#define DUMP_ROW_LEN  8 /* how many bytes per row when dumping pkt */
#define DUMP_OFF_LEN  5 /* how many digits to use for the offset */

#ifndef VERBOSE
#  define QUIET
#endif

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

		fprintf (stderr, "%s\n", buf);
	}
	fprintf (stderr, "\n");
}

static void
int_hn (int sig)
{
	interrupted = 1;
}

int
main (int argc, char** argv)
{
	int rc;
	if (argc != 2)
		return -1;

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
	int capfd = open (argv[1], O_RDONLY);
	if (capfd == -1)
	{
		perror ("Cannot open file");
		return -1;
	}

	struct pollfd pfd;
	pfd.fd = nmd->fd;
	pfd.events = POLLOUT;

	int looped = 0;
	uint64_t p = 0;
	uint64_t mcas = 0, ticks = 0, peaks = 0, areas = 0, pulses = 0,
		     traces = 0, trace_sgls = 0, trace_avgs = 0, trace_dps = 0,
		     trace_dp_trs = 0;
	uint64_t missed = 0, invalid = 0;
	uint16_t prev_fseq, mca_n = 0, trace_n = 0;

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

		tespkt pkt;
		memset (&pkt, 0, sizeof (pkt));

		/* Read the header */
		rc = read (capfd, &pkt, TES_HDR_LEN);
		if (rc == -1)
		{
			perror ("Could not read in header");
			break;
		}
		else if (rc == 0)
		{ /* reached EOF */
			printf ("\nReached EOF\n");
			if (looped == 0)
				printf ("packets: %lu\n"
					"missed:  %lu\n"
					"invalid: %lu\n"
					"mcas:    %lu\n"
					"ticks:   %lu\n"
					"peaks:   %lu\n"
					"areas:   %lu\n"
					"pulses:  %lu\n"
					"traces:  %lu\n"
					"  sgl:   %lu\n"
					"  avg:   %lu\n"
					"  dp:    %lu\n"
					"  dptr:  %lu\n",
					p,
					missed,
					invalid,
					mcas,
					ticks,
					peaks,
					areas,
					pulses,
					traces,
					trace_sgls,
					trace_avgs,
					trace_dps,
					trace_dp_trs);

			p = 0;
			looped++;
			continue;
		}
		else if (rc != TES_HDR_LEN)
		{
			fprintf (stderr,
				"Read unexpected number of bytes "
				"from header: %d, packet no. %lu\n", rc, p + 1);
			break;
		}
		p++;

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
				"from payload: %d, packet no. %lu\n", rc, p);
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

		if (p % 50 == 0)
			poll (NULL, 0, 1);
		rc = nm_inject (nmd, &pkt, len);
		if (!rc)
		{
		        fprintf (stderr, "Cannot inject packet\n");
		        break;
		}

		if (looped > 0)
			continue;

		/* Statistics, only first time through the loop */
		uint16_t cur_fseq = tespkt_fseq (&pkt);
		if (p > 1)
			missed += (uint16_t)(cur_fseq - prev_fseq - 1);
		prev_fseq = cur_fseq;

		rc = tespkt_is_valid (&pkt);
		if (rc)
		{
#ifndef QUIET
			fprintf (stderr, "Packet no. %d: ", p);
			tespkt_perror (stderr, rc);
			dump_pkt ((void*)&pkt, TES_HDR_LEN + 8);
#endif
			invalid++;
		}

		const char* ptype = NULL;
		uint16_t* prev_n = NULL;
		if (tespkt_is_mca (&pkt))
		{
			mcas++;
			ptype = "MCA";
			prev_n = &mca_n;
		}
		else if (tespkt_is_tick (&pkt))
			ticks++;
		else if (tespkt_is_peak (&pkt))
			peaks++;
		else if (tespkt_is_area (&pkt))
			areas++;
		else if (tespkt_is_pulse (&pkt))
			pulses++;
		else if (tespkt_is_trace (&pkt))
		{
			traces++;
			if (tespkt_is_trace_dp (&pkt))
			{
				trace_dps++;
			}
			else
			{
				prev_n = &trace_n;
				if (tespkt_is_trace_sgl (&pkt))
				{
					trace_sgls++;
					ptype = "Trace single";
				}
				else if (tespkt_is_trace_avg (&pkt))
				{
					trace_avgs++;
					ptype = "Trace avg";
				}
				else if (tespkt_is_trace_dptr (&pkt))
				{
					trace_dp_trs++;
					ptype = "Trace DP trace";
				}
			}
		}

		if (prev_n != NULL)
		{ /* packet is part of a multi-frame, i.e. trace or MCA */
			if (tespkt_is_header (&pkt))
			{
				assert (ptype != NULL);
#ifndef QUIET
				off_t pos = lseek (capfd, 0, SEEK_CUR);
				printf ("Packet no. %lu (ends at offset 0x%lx): "
						"new event stream for type %s, "
						"previous count was %hu\n",
						p, pos, ptype, *prev_n);
#endif
				*prev_n = 0;
			}
			(*prev_n)++;
		}
	}

	close (capfd);
	nm_close (nmd);
	return 0;
}
