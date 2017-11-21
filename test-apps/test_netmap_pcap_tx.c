#include <pcap/pcap.h>
#include "net/tespkt.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <assert.h>

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>

#define NM_IFNAME "vale:tes{1"
#define PCAPFILE  "noise drive.pcapng"
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
	assert (nmd->first_tx_ring == nmd->last_tx_ring);
	struct netmap_ring* txring = NETMAP_TXRING (
			nmd->nifp, nmd->cur_tx_ring);

	/* Open the pcap file */
	char err[PCAP_ERRBUF_SIZE + 1];
	pcap_t* pc = pcap_open_offline (PCAPFILE, err);
	if (pc == NULL)
	{
		fprintf (stderr, "Cannot open pcap file: %s", err);
		return -1;
	}
	struct pcap_pkthdr h;

	struct pollfd pfd;
	pfd.fd = nmd->fd;
	pfd.events = POLLOUT;

	uint64_t p = 0;
	int sf = -1;
	uint16_t ef;
	uint64_t missed = 0;
	int looped = 0;

	while (!interrupted && looped < 1)
	{
		const unsigned char* pkt = pcap_next (pc, &h);
		if (pkt == NULL)
		{
			if (looped == 0)
			{
				printf ("\n----------\n"
					"Total number of packets: %lu\n"
					"Missed packets:          %lu\n"
					"Start frame:             %d\n"
					"End frame:               %hu\n",
					p, missed, sf, ef);
			}
			/* Reopen the file */
			pcap_close (pc);
			pcap_t* pc = pcap_open_offline (PCAPFILE, err);
			if (pc == NULL)
			{
				fprintf (stderr, "Cannot open pcap file: %s", err);
				return -1;
			}
			looped++;
			p = 0;
			continue;
		}
		p++;

		/* Send the packet */
		uint16_t len = tespkt_flen ((tespkt*)pkt);
		if (len != h.len && len >= 60)
			printf ("Packet #%5lu: frame len says %5hu, "
				"caplen = %5hu, len = %5hu\n",
				p, len, h.caplen, h.len);
		if (len > MAX_TES_FRAME_LEN)
			len = MAX_TES_FRAME_LEN;
		rc = poll (&pfd, 1, -1);
		if (rc == -1)
		{
			if (errno != EINTR)
				perror ("poll");
			break;
		}
		// rc = nm_inject (nmd, pkt, len);
		// if (!rc)
		// {
		//         fprintf (stderr, "Cannot inject packet\n");
		//         break;
		// }

		if (looped)
			continue;

		/* Statistics */
		if (sf == -1)
		{
			sf = (int)tespkt_fseq ((tespkt*)pkt);
		}
		else
		{
			missed += (uint64_t) (
				(uint16_t)(tespkt_fseq ((tespkt*)pkt) - ef) - 1 );
		}
		ef = tespkt_fseq ((tespkt*)pkt);
		pkt_pretty_print ((tespkt*)pkt, stdout, stderr);
		tespkt_perror (stdout, tespkt_is_valid ((tespkt*)pkt));
		printf ("\n");
		// dump_pkt (pkt, TES_HDR_LEN);
	}

	pcap_close (pc);
	return 0;
}
