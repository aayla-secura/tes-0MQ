/*
 * TO FIX:
 * MCA header not correctly constructed?
 */

#define TESPKT_DEBUG
#include "net/tespkt_gen.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <assert.h>

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>

#include <net/ethernet.h>
#ifdef linux
# include <netinet/ether.h>
#endif
#define DST_HW_ADDR "ff:ff:ff:ff:ff:ff"
#define SRC_HW_ADDR "5a:ce:be:b7:b2:91"

#ifndef NUM_LOOPS
#  define NUM_LOOPS INT_MAX
#endif
#ifndef WAIT_EVERY
#  define WAIT_EVERY 50 /* will poll for 1ms every that many packets */
#endif
#ifndef NM_IFNAME
#  define NM_IFNAME "vale0:vi0"
#endif
#define DUMP_ROW_LEN 16 /* how many bytes per row when dumping pkt */
#define DUMP_OFF_LEN  5 /* how many digits to use for the offset */

#ifndef PATH_MAX
#  ifdef MAXPATHLEN
#    define PATH_MAX MAXPATHLEN
#  else
#    define PATH_MAX 4096
#  endif
#endif

#define TESPKT_LEN
// from tesd_task_save.c
struct s_ftype_t
{
	/* PT: */
#define FTYPE_PEAK        0
#define FTYPE_AREA        1
#define FTYPE_PULSE       2
#define FTYPE_TRACE_SGL   3
#define FTYPE_TRACE_AVG   4
#define FTYPE_TRACE_DP    5
#define FTYPE_TRACE_DP_TR 6
#define FTYPE_TICK        7
#define FTYPE_MCA         8
#define FTYPE_BAD         9
	uint8_t PT  : 4;
	uint8_t     : 2; /* reserved */
	uint8_t HDR : 1; /* header frame in multi-frame stream */
	uint8_t SEQ : 1; /* sequence error in event stream */
};
#define FIDX_LEN 16         // frame index
struct s_fidx_t
{
	uint64_t start;   // frame's offset into its dat file
	uint32_t length;  // payload's length
	uint16_t esize;   // original event size
	uint8_t  changed; // event frame differs from previous
	struct s_ftype_t ftype; // see definition of struct
};

static bool interrupted = false;

struct s_stats_t
{
	uint64_t pkts;
	uint64_t mcas;
	uint64_t ticks;
	uint64_t peaks;
	uint64_t areas;
	uint64_t pulses;
	uint64_t traces;
	uint64_t trace_sgls;
	uint64_t trace_avgs;
	uint64_t trace_dps;
	uint64_t trace_dp_trs;
	uint64_t missed;
	uint64_t invalid;
	uint16_t prev_fseq;
	uint16_t prev_pseq;
	uint16_t mca_n;
	uint16_t trace_n;
};

static void
s_dump_pkt (const unsigned char* pkt, uint32_t len)
{
	char buf[ 4*DUMP_ROW_LEN + DUMP_OFF_LEN + 2 + 1 ] = {0};

	for (uint32_t r = 0; r < len; r += DUMP_ROW_LEN)
	{
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
s_print_stats (struct s_stats_t* stats)
{
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
		stats->pkts,
		stats->missed,
		stats->invalid,
		stats->mcas,
		stats->ticks,
		stats->peaks,
		stats->areas,
		stats->pulses,
		stats->traces,
		stats->trace_sgls,
		stats->trace_avgs,
		stats->trace_dps,
		stats->trace_dp_trs);
}

static void
s_update_stats (tespkt* pkt, struct s_stats_t* stats)
{
		uint16_t cur_fseq = tespkt_fseq (pkt);
		if (stats->pkts > 1)
			stats->missed += (uint16_t)(cur_fseq - stats->prev_fseq - 1);
		stats->prev_fseq = cur_fseq;
		stats->prev_pseq = tespkt_pseq (pkt);

		int rc = tespkt_is_valid (pkt);
		if (rc)
		{
#ifdef VERBOSE
			fprintf (stderr, "Packet no. %lu: ", stats->pkts);
			tespkt_perror (stderr, rc);
			s_dump_pkt ((void*)pkt, TESPKT_HDR_LEN +
				(tespkt_is_mca (pkt) && tespkt_is_header (pkt) ? 40 : 8));
#endif
			stats->invalid++;
		}

		const char* ptype = NULL;
		uint16_t* prev_n = NULL;
		if (tespkt_is_mca (pkt))
		{
			stats->mcas++;
			ptype = "MCA";
			prev_n = &stats->mca_n;
		}
		else if (tespkt_is_tick (pkt))
			stats->ticks++;
		else if (tespkt_is_peak (pkt))
			stats->peaks++;
		else if (tespkt_is_area (pkt))
			stats->areas++;
		else if (tespkt_is_pulse (pkt))
			stats->pulses++;
		else if (tespkt_is_trace (pkt))
		{
			stats->traces++;
			if (tespkt_is_trace_dp (pkt))
			{
				stats->trace_dps++;
			}
			else
			{
				prev_n = &stats->trace_n;
				if (tespkt_is_trace_sgl (pkt))
				{
					stats->trace_sgls++;
					ptype = "Trace single";
				}
				else if (tespkt_is_trace_avg (pkt))
				{
					stats->trace_avgs++;
					ptype = "Trace avg";
				}
				else if (tespkt_is_trace_dptr (pkt))
				{
					stats->trace_dp_trs++;
					ptype = "Trace DP trace";
				}
			}
		}

		if (prev_n != NULL)
		{ /* packet is part of a multi-frame, i.e. trace or MCA */
			if (tespkt_is_header (pkt))
			{
				assert (ptype != NULL);
				*prev_n = 0;
			}
			(*prev_n)++;
		}
}

static void
s_int_hn (int sig)
{
	interrupted = true;
}

static int
s_fopen (const char* basefname, const char* ext)
{
	char tmpfname[PATH_MAX] = {0};

	/* Index */
	int rc = snprintf (tmpfname, PATH_MAX, "%s%s%s",
		basefname,
		ext != NULL ? "." : "",
		ext != NULL ? ext : "");
	if (rc < 0)
	{
		perror ("snprintf base filename");
		return -1;
	}
	else if ((size_t)rc >= PATH_MAX)
	{
		fprintf (stderr, "base filename too long\n");
		return -1;
	}
	int fd = open (tmpfname, O_RDONLY);
	if (fd == -1)
	{
		fprintf (stderr, "Cannot open %s\n", tmpfname);
		if (errno != 0)
			perror ("");
		return -1;
	}

	return fd;
}

static int
s_inject_from_fidx (const char* basefname,
		struct nm_desc* nmd, off_t skip)
{
	int fidxfd = s_fopen (basefname, "fidx");
	int tdatfd = s_fopen (basefname, "tdat");
	int mdatfd = s_fopen (basefname, "mdat");
	int edatfd = s_fopen (basefname, "edat");
	if (fidxfd == -1 || tdatfd == -1 || mdatfd == -1 || edatfd == -1)
	{
		close (fidxfd);
		close (tdatfd);
		close (mdatfd);
		close (edatfd);
		return -1;
	}

	struct pollfd pfd;
	pfd.fd = nmd->fd;
	pfd.events = POLLOUT;

	int looped = 0;
	struct s_stats_t stats = {0};

	tespkt* pkt = (tespkt*) malloc (TESPKT_MTU);
	if (pkt == NULL)
	{
		perror ("");
		return -1;
	}
	memset (pkt, 0, sizeof (tespkt));
	while (!interrupted && looped != NUM_LOOPS)
	{
		int rc;
		if (stats.pkts == 0)
		{
			/* seek to BOF + skip */
			rc = lseek (fidxfd, skip, SEEK_SET);
			if ((off_t)rc == (off_t)-1)
			{
				perror ("Could not seek to BOF");
				break;
			}
			else if ((off_t)rc != skip)
			{
				fprintf (stderr, "Could not seek to BOF\n");
				break;
			}
		}

		struct s_fidx_t fidx = {0};

		/* Read the index */
		rc = read (fidxfd, &fidx, FIDX_LEN);
		if (rc == -1)
		{
			perror ("Could not read in index");
			break;
		}
		else if (rc == 0)
		{ /* reached EOF */
			if (looped == 0)
				s_print_stats (&stats);

			stats.pkts = 0;
			looped++;
			continue;
		}
		else if (rc != FIDX_LEN)
		{
			fprintf (stderr,
				"Read unexpected number of bytes "
				"from index: %d, packet no. %lu\n", rc, stats.pkts + 1);
			break;
		}
		stats.pkts++;

		/* Construct the ethernet header */
		uint16_t plen = fidx.length;
		assert (plen <= TESPKT_MTU - TESPKT_HDR_LEN);
		tespkt_set_len (pkt, TESPKT_HDR_LEN + plen);
		struct ether_addr* mac_addr = ether_aton (DST_HW_ADDR);
		memcpy (&pkt->eth_hdr.ether_dhost, mac_addr, ETHER_ADDR_LEN);
		mac_addr = ether_aton (SRC_HW_ADDR);
		memcpy (&pkt->eth_hdr.ether_shost, mac_addr, ETHER_ADDR_LEN);
		int datfd = -1;
		switch (fidx.ftype.PT)
		{
			case FTYPE_PEAK:
			case FTYPE_AREA:
			case FTYPE_PULSE:
			case FTYPE_TRACE_SGL:
			case FTYPE_TRACE_AVG:
			case FTYPE_TRACE_DP:
			case FTYPE_TRACE_DP_TR:
				pkt->eth_hdr.ether_type = htons (ETHERTYPE_F_EVENT);
				datfd = edatfd;
				break;
			case FTYPE_TICK:
				pkt->eth_hdr.ether_type = htons (ETHERTYPE_F_EVENT);
				datfd = tdatfd;
				break;
			case FTYPE_MCA:
				pkt->eth_hdr.ether_type = htons (ETHERTYPE_F_MCA);
				datfd = mdatfd;
				break;
		}
		if (datfd == -1) /* BAD frame */
			continue;

		/* Construct the tes header */
		pkt->tes_hdr.esize = fidx.esize; /* it's in FPGA byte-order */
		memset (&pkt->tes_hdr.etype, 0, sizeof (pkt->tes_hdr.etype));
		switch (fidx.ftype.PT)
		{
			case FTYPE_PEAK:
			case FTYPE_AREA:
			case FTYPE_PULSE:
				pkt->tes_hdr.etype.PKT = fidx.ftype.PT;
				break;
			case FTYPE_TRACE_SGL:
			case FTYPE_TRACE_AVG:
			case FTYPE_TRACE_DP:
			case FTYPE_TRACE_DP_TR:
				pkt->tes_hdr.etype.PKT = TESPKT_TYPE_TRACE;
				pkt->tes_hdr.etype.TR = fidx.ftype.PT - 3;
				break;
			case FTYPE_TICK:
				pkt->tes_hdr.etype.T = 1;
				break;
		}

		/* If sequence error (SEQ == 1), assume one missed */
		if (stats.pkts == 1)
			pkt->tes_hdr.fseq = 0;
		else
			tespkt_set_fseq (pkt, stats.prev_fseq + 1 + fidx.ftype.SEQ);

		bool is_mca = tespkt_is_mca (pkt);
		bool is_trace = ( tespkt_is_trace (pkt) &&
			! tespkt_is_trace_dp (pkt) );
		if (fidx.ftype.HDR || ( ! is_trace && ! is_mca ))
			pkt->tes_hdr.pseq = stats.prev_pseq = 0; /* short event or header */
		else
		{
			stats.prev_pseq += 1 + fidx.ftype.SEQ;
			tespkt_set_pseq (pkt, stats.prev_pseq);
		}

		/* Read the payload */
		rc = lseek (datfd, fidx.start, SEEK_SET);
		if ((off_t)rc == (off_t)-1)
		{
			perror ("Could not seek to payload");
			break;
		}
		else if ((uint64_t)rc != fidx.start)
		{
			fprintf (stderr, "Could not seek to payload\n");
			break;
		}
		rc = read (datfd, (char*)pkt + TESPKT_HDR_LEN, plen);
		if (rc == -1)
		{
			perror ("Could not read in payload");
			break;
		}
		else if (rc != plen)
		{
			fprintf (stderr,
				"Read unexpected number of bytes "
				"from payload: %d, packet no. %lu\n", rc, stats.pkts);
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

		if (stats.pkts % WAIT_EVERY == 0)
			poll (NULL, 0, 1);
		rc = nm_inject (nmd, pkt, plen + TESPKT_HDR_LEN);
		if (!rc)
		{
			fprintf (stderr, "Cannot inject packet\n");
			break;
		}

		if (looped > 0)
			continue;

		/* Statistics, only first time through the loop */
		s_update_stats (pkt, &stats);
	}

	close (fidxfd);
	close (tdatfd);
	close (mdatfd);
	close (edatfd);
	return (looped == NUM_LOOPS ? 0 : -1);
}

static int
s_inject_from_flat (const char* filename,
		struct nm_desc* nmd, off_t skip)
{
	int capfd = s_fopen (filename, NULL);
	if (capfd == -1)
		return -1;

	struct pollfd pfd;
	pfd.fd = nmd->fd;
	pfd.events = POLLOUT;

	int looped = 0;
	struct s_stats_t stats = {0};

	while (!interrupted && looped != NUM_LOOPS)
	{
		int rc;
		if (stats.pkts == 0)
		{
			/* seek to BOF + skip */
			rc = lseek (capfd, skip, SEEK_SET);
			if (rc == -1)
			{
				perror ("Could not seek to BOF");
				break;
			}
			else if ((off_t)rc != skip)
			{
				fprintf (stderr, "Could not seek to BOF\n");
				break;
			}
		}

		tespkt* pkt = (tespkt*) malloc (TESPKT_MTU);
		if (pkt == NULL)
		{
			perror ("");
			exit (EXIT_FAILURE);
		}
		memset (pkt, 0, sizeof (tespkt));

		/* Read the header */
		rc = read (capfd, (char*)pkt, TESPKT_HDR_LEN);
		if (rc == -1)
		{
			perror ("Could not read in header");
			break;
		}
		else if (rc == 0)
		{ /* reached EOF */
			if (looped == 0)
				s_print_stats (&stats);

			stats.pkts = 0;
			looped++;
			continue;
		}
		else if (rc != TESPKT_HDR_LEN)
		{
			fprintf (stderr,
				"Read unexpected number of bytes "
				"from header: %d, packet no. %lu\n", rc, stats.pkts + 1);
			break;
		}
		stats.pkts++;

		/* Read the payload */
		uint16_t len = tespkt_flen (pkt);
		assert (len <= TESPKT_MTU);
		assert (len > TESPKT_HDR_LEN);
		rc = read (capfd, (char*)pkt + TESPKT_HDR_LEN,
			len - TESPKT_HDR_LEN);
		if (rc == -1)
		{
			perror ("Could not read in payload");
			break;
		}
		else if (rc != len - TESPKT_HDR_LEN)
		{
			fprintf (stderr,
				"Read unexpected number of bytes "
				"from payload: %d, packet no. %lu\n", rc, stats.pkts);
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

		if (stats.pkts % WAIT_EVERY == 0)
			poll (NULL, 0, 1);
		rc = nm_inject (nmd, pkt, len);
		if (!rc)
		{
			fprintf (stderr, "Cannot inject packet\n");
			break;
		}

		if (looped > 0)
			continue;

		/* Statistics, only first time through the loop */
		s_update_stats (pkt, &stats);
	}

	close (capfd);
	return (looped == NUM_LOOPS ? 0 : -1);
}

int
main (int argc, char* argv[])
{
	tespkt_self_test ();

	int rc;
	/* TO DO: support single .adat with a frame index and automatically
	 * detect if headers are saved (from the length of a tick) */
	if (argc != 2)
		return -1;

	/* Signal handlers */
	struct sigaction sigact;
	sigact.sa_flags = 0;
	sigact.sa_handler = s_int_hn;
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
	char* filename = argv[1];
	if (strlen (filename) >= PATH_MAX)
		return -1;
	char* ext = strrchr (filename, '.');
	if (ext != NULL && strcmp (ext, ".fidx") == 0)
	{ /* read from index and reconstruct headers,
	   * data file must NOT contain headers */
		size_t baselen = ext - filename;
		char basefname[PATH_MAX] = {0};
		rc = snprintf (basefname, baselen + 1, "%s", filename);
		if (rc < 0)
		{
			perror ("snprintf base filename");
			return -1;
		}
		assert (strlen (basefname) == baselen);
		rc = s_inject_from_fidx (basefname, nmd, 0);
	}
	else
	{ /* no index file, data file must contain headers */
		rc = s_inject_from_flat (filename, nmd, 0);
	}

	nm_close (nmd);
	return rc;
}
