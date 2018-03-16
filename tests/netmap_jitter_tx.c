#ifdef linux
/* ppoll requires it on linux */
#  define _GNU_SOURCE
#  include <signal.h>
#endif
#include <stdio.h>
#include <stdlib.h>
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

#define PKT_LEN (TESPKT_HDR_LEN + TESPKT_TICK_HDR_LEN)
#ifndef NMIF
#  define NMIF "vale0:vi0"
#endif
#define DUMP_ROW_LEN   16 /* how many bytes per row when dumping pkt */
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
#  define DELAY(x) ((int) ( \
				(double)rand () * MAX_DELAY / RAND_MAX))
#else
#  define RAND_CH_EVERY 10000
static uint16_t delays[MAX_NUM_CHANNELS] = {10, 20, 10, 5, 10, 5, 5, 15};
#  define DELAY(x) delays[x]
#endif

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

int interrupted = 0;
void
int_hn (int sig)
{
	interrupted = 1;
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
	tespkt* pkt = (tespkt*) malloc (PKT_LEN);
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
	tespkt_set_len (pkt, PKT_LEN);
	tespkt_set_esize (pkt, 1);

	struct timespec twait = {
		.tv_sec = WAIT_SEC,
		.tv_nsec = WAIT_NSEC
	};

	/* Poll */
	struct pollfd pfd;
	pfd.fd = nmd->fd;
	pfd.events = POLLOUT;

	int ch = 0;
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
			dump_pkt ((void*)pkt, TESPKT_HDR_LEN + 8);
			break;
		}

#if WAIT_SEC > 0
		tespkt_pretty_print (pkt, stdout, stderr);
#endif
		if (nm_inject (nmd, pkt, PKT_LEN))
		{
			tespkt_inc_fseq (pkt, 1);

			/* Toss a coin for tick vs non-tick */
			struct tespkt_event_type* et = tespkt_etype (pkt);
			if ( (int) ((double) rand () * TICK_EVERY / RAND_MAX) == 0 )
			{
				et->T = 1;
				tespkt_set_esize (pkt, 3);
				assert (tespkt_event_nums (pkt) == 1);
			}
			else
			{
				et->T = 0;
				tespkt_set_esize (pkt, 1);
			}

			for (int e = 0; e < tespkt_event_nums (pkt); e++)
			{
				ch++;
				if ((int)((double)rand() * RAND_CH_EVERY / RAND_MAX) == 0 )
					ch = (int)((double)rand() * NUM_CHANNELS / RAND_MAX);
				/* rand () gave RAND_MAX or ch++ wrapped around */
				if (ch == NUM_CHANNELS)
					ch = 0;
				assert (ch < NUM_CHANNELS);

				struct tespkt_event_hdr* eh =
					(struct tespkt_event_hdr*)((char*)&pkt->body +
					e*tespkt_true_esize (pkt));
				/* FIX: should ticks be the same channel all the time */
				eh->flags.CH = ch;

				eh->toff = DELAY(ch);
			}
		}
	}

	nm_close (nmd);
	return 0;
}
