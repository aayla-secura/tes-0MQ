#ifdef linux
/* ppoll requires it on linux */
#  define _GNU_SOURCE
#  include <signal.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
/* #include <sys/time.h> */
#include <errno.h>
#include <assert.h>
#include <poll.h>
#include <net/ethernet.h>
#ifdef linux
# include <netinet/ether.h>
#endif

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>

#define TESPKT_DEBUG
#include "net/tespkt_gen.h"

#define DST_HW_ADDR "ff:ff:ff:ff:ff:ff"
#define SRC_HW_ADDR "5a:ce:be:b7:b2:91"
#define ETH_PROTO ETHERTYPE_F_EVENT

#ifndef NMIF
#  define NMIF "vale0:vi0"
#endif
#define DUMP_ROW_LEN    8 /* how many bytes per row when dumping pkt */
#define DUMP_OFF_LEN    5 /* how many digits to use for the offset */
#define TICK_EVERY 50
#define WAIT_NSEC 10000000
#define WAIT_SEC 0
#include "api.h"
#define NUM_CHANNELS (TES_JITTER_NHISTS + 1)
#define MAX_NUM_CHANNELS 8

#ifdef RANDOM
#  define RAND_CH_EVERY 0
#  define MAX_DELAY 500
#  define DELAY(x) ((uint16_t) ( \
				(double)rand () * MAX_DELAY / RAND_MAX))
#else
#  define RAND_CH_EVERY 10000
static uint16_t delays[MAX_NUM_CHANNELS] = {10, 20, 10, 5, 10, 5, 5, 15};
#  define DELAY(x) delays[x]
#endif

#define MAX_AREA UINT32_MAX
#define MAX_HEIGHT UINT16_MAX
#define MAX_DP UINT32_MAX /* it's 6 bytes, nevermind */

#define TICK_LEN (TESPKT_HDR_LEN + TESPKT_TICK_HDR_LEN)
#define DP_LEN   (TESPKT_HDR_LEN + \
  TESPKT_TRACE_FULL_HDR_LEN + \
	TESPKT_PEAK_LEN + 8) /* dot prod is 8 bytes */

static void
dump_pkt (const unsigned char* pkt, uint32_t len)
{
	char buf[ 4*DUMP_ROW_LEN + DUMP_OFF_LEN + 2 + 1 ] = {0};

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

static bool interrupted = false;
static void
int_hn (int sig)
{
	interrupted = true;
}

int
main (void)
{
	srand (time (NULL));
	int rc;

	/* Signal handlers */
	struct sigaction sigact;

	sigact.sa_flags = 0;
	sigact.sa_handler = int_hn;
	sigemptyset (&sigact.sa_mask);
	sigaddset (&sigact.sa_mask, SIGINT);
	sigaddset (&sigact.sa_mask, SIGTERM);
	sigaddset (&sigact.sa_mask, SIGALRM);
	rc = sigaction (SIGINT, &sigact, NULL);
	rc |= sigaction (SIGTERM, &sigact, NULL);
	if (rc != 0)
	{
		perror ("sigaction");
		exit (EXIT_FAILURE);
	}

	struct nm_desc* nmd = nm_open(NMIF, NULL, 0, NULL);
	if (nmd == NULL)
	{
		perror ("Could not open interface");
		exit (EXIT_FAILURE);
	}

	/* A dummy packet */
	tespkt* pkt = (tespkt*) malloc (TESPKT_MTU);
	if (pkt == NULL)
	{
		perror ("");
		exit (EXIT_FAILURE);
	}
	memset (pkt, 0, sizeof (tespkt));
	tespkt_set_type_evt (pkt);
	struct ether_addr* mac_addr = ether_aton (DST_HW_ADDR);
	memcpy (&pkt->eth_hdr.ether_dhost, mac_addr, ETHER_ADDR_LEN);
	mac_addr = ether_aton (SRC_HW_ADDR);
	memcpy (&pkt->eth_hdr.ether_shost, mac_addr, ETHER_ADDR_LEN);
	pkt->eth_hdr.ether_type = htons (ETH_PROTO);

	struct timespec twait = {
		.tv_sec = WAIT_SEC,
		.tv_nsec = WAIT_NSEC
	};

	/* Poll */
	struct pollfd pfd;
	pfd.fd = nmd->fd;
	pfd.events = POLLOUT;

	int ch = 0;
	tespkt_set_len (pkt, TICK_LEN);
	tespkt_set_etype_tick (pkt);
	while ( ! interrupted )
	{
#if (WAIT_NSEC > 0)
		ppoll (NULL, 0, &twait, NULL);
#endif
		rc = poll (&pfd, 1, 1000);
		if (rc == -1)
			break;
		else if (rc == 0)
			continue; /* timed out */

		rc = tespkt_is_valid (pkt);
		if (rc != 0)
		{
			tespkt_pretty_print (pkt, stdout, stderr);
			tespkt_perror (stderr, rc);
			dump_pkt ((void*)pkt, DP_LEN + 8);
			break;
		}

#if WAIT_SEC > 0
		tespkt_pretty_print (pkt, stdout, stderr);
#endif
		if (nm_inject (nmd, pkt, TESPKT_MTU) == 0)
			continue; /* try again */

		tespkt_inc_fseq (pkt, 1);
		memset (&pkt->body, 0, TESPKT_MTU - TESPKT_HDR_LEN);

		/* Toss a coin for tick vs non-tick */
		assert (tespkt_event_nums (pkt) == 1);
		if ( (int) ((double) rand () * TICK_EVERY / RAND_MAX) == 0 )
		{
			tespkt_set_etype_tick (pkt);
			tespkt_set_len (pkt, TICK_LEN);
		}
		else
		{
			tespkt_set_etype_trace (pkt, TESPKT_TRACE_TYPE_DP);
			tespkt_set_len (pkt, DP_LEN);
		}

		ch++;
		if ((int)((double)rand() * RAND_CH_EVERY / RAND_MAX) == 0 )
			ch = (int)((double)rand() * NUM_CHANNELS / RAND_MAX);
		/* rand () gave RAND_MAX or ch++ wrapped around */
		if (ch == NUM_CHANNELS)
			ch = 0;
		assert (ch < NUM_CHANNELS);

		struct tespkt_trace_full_hdr* th =
			(struct tespkt_trace_full_hdr*) tespkt_ehdr (pkt, 0);
		/* FIX: should ticks be the same channel all the time */
		th->trace.flags.CH = ch;

		th->trace.toff = DELAY(ch);
		if (tespkt_is_tick (pkt))
			continue;

#if TES_VERSION < 2
		th->trace.flags.PC = 1;
#else
		th->trace.flags.PC = 0;
#endif
		th->trace.size = htofs (32);

		assert (tespkt_peak_nums (pkt, 0) == 1);
		struct tespkt_peak* ph = tespkt_peak (pkt, 0, 0);
		struct tespkt_dot_prod* dh =
			(struct tespkt_dot_prod*)((char*)ph + TESPKT_PEAK_LEN);

		uint32_t area = ((uint32_t) (
			(double)rand () * MAX_AREA / RAND_MAX));
		uint32_t height = ((uint32_t) (
			(double)rand () * MAX_HEIGHT / RAND_MAX));
		uint64_t dp = ((uint32_t) (
			(double)rand () * MAX_DP / RAND_MAX));
		
		th->pulse.area = area;
		ph->height = height;
		dh->dot_prod = dp;
	}

	nm_close (nmd);
	return 0;
}
