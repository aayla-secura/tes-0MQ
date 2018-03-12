#ifdef linux
/* ppoll requires it on linux */
#  define _GNU_SOURCE
#  include <signal.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
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
#include "net/tespkt.h"

#define DST_HW_ADDR "ff:ff:ff:ff:ff:ff"
#define SRC_HW_ADDR "5a:ce:be:b7:b2:91"
#define ETH_PROTO ETHERTYPE_F_EVENT

#define PKT_LEN (TES_HDR_LEN + TICK_HDR_LEN)
#ifndef NMIF
#  define NMIF "vale0:vi0"
#endif
#define DUMP_ROW_LEN   16 /* how many bytes per row when dumping pkt */
#define DUMP_OFF_LEN    5 /* how many digits to use for the offset */
#define TICK_EVERY 1000
#define FORCE_SAME_EVERY 100
#define MAX_DELAY 500
#define WAIT_NSEC 1000000

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
	struct tespkt pkt = {0};
	struct ether_addr* mac_addr = ether_aton (DST_HW_ADDR);
	memcpy (&pkt.eth_hdr.ether_dhost, mac_addr, ETHER_ADDR_LEN);
	mac_addr = ether_aton (SRC_HW_ADDR);
	memcpy (&pkt.eth_hdr.ether_shost, mac_addr, ETHER_ADDR_LEN);
	pkt.eth_hdr.ether_type = htons (ETH_PROTO);
	pkt.length = PKT_LEN;
	pkt.tes_hdr.esize = 1;

	struct timespec twait = {
		.tv_sec = 0,
		.tv_nsec = WAIT_NSEC
	};

	/* Poll */
	struct pollfd pfd;
	pfd.fd = nmd->fd;
	pfd.events = POLLOUT;

	int next_is_nonref = 1;
	while ( ! interrupted )
	{
		rc = poll (&pfd, 1, 1000);
		if (rc == -1)
			break;
		else if (rc == 0)
			continue; /* timed out */

		rc = tespkt_is_valid (&pkt);
		if (rc != 0)
		{
			/* pkt_pretty_print (&pkt, stdout, stderr); */
			tespkt_perror (stderr, rc);
			dump_pkt ((void*)&pkt, TES_HDR_LEN + 8);
			break;
		}

		if (nm_inject (nmd, &pkt, PKT_LEN))
		{
			pkt.tes_hdr.fseq++;

			/* Toss a coin for tick vs non-tick */
			struct tespkt_event_type* et = tespkt_etype (&pkt);
			if ( (int) ((double) rand () * TICK_EVERY / RAND_MAX) == 0 )
			{
				et->T = 1;
				pkt.tes_hdr.esize = 3;
			}
			else
			{
				et->T = 0;
				pkt.tes_hdr.esize = 1;
			}

			/* Toss a coin for channel 0 vs 1 */
			struct tespkt_event_hdr* eh =
				(struct tespkt_event_hdr*)(void*) &pkt.body;
			uint8_t ch = next_is_nonref;
			if ((int)((double)rand() * FORCE_SAME_EVERY / RAND_MAX) != 0 )
				next_is_nonref ^= 1;
			eh->flags.CH = ch;

			/* Get random delay */
			uint16_t delay	= (int) (
				(double)rand () * MAX_DELAY / RAND_MAX);
			eh->toff = delay;
		}

#if (WAIT_NSEC > 0)
		ppoll (NULL, 0, &twait, NULL);
#endif
	}

	nm_close (nmd);
	return 0;
}
