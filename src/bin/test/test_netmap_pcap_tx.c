#include <pcap/pcap.h>
#include "net/fpgapkt.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define PCAPFILE "/home/aleksandrina/FPGA/noise drive.pcapng"
#define DUMP_ROW_LEN   16 /* how many bytes per row when dumping pkt */
#define DUMP_OFF_LEN    5 /* how many digits to use for the offset */

static void
dump_pkt (const unsigned char* pkt)
{
	u_int16_t len = pkt_len ((fpga_pkt*)pkt);
	char buf[ 4*DUMP_ROW_LEN + DUMP_OFF_LEN + 2 + 1 ];

	memset (buf, 0, sizeof (buf));
	for (int r = 0; r < len; r += DUMP_ROW_LEN) {
		sprintf (buf, "%0*x: ", DUMP_OFF_LEN, r);

		/* hexdump */
		for (int b = 0; b < DUMP_ROW_LEN && b+r < len; b++)
			sprintf (buf + DUMP_OFF_LEN + 2 + 3*b, "%02x ",
				(u_int8_t)(pkt[b+r]));

		/* ASCII dump */
		for (int b = 0; b < DUMP_ROW_LEN && b+r < len; b++)
			sprintf (buf + DUMP_OFF_LEN + 2 + b + 3*DUMP_ROW_LEN,
				"%c", isprint (pkt[b+r]) ? pkt[b+r] : '.');

		printf ("%s\n", buf);
	}
	puts ("");
}
int main (void)
{
	char err[PCAP_ERRBUF_SIZE+1];
	pcap_t* pc = pcap_open_offline (PCAPFILE, err);
	if (pc == NULL)
	{
		fprintf (stderr, "Cannot open pcap file: %s", err);
		return -1;
	}

	struct pcap_pkthdr h;
	uint64_t p = 0;
	int sf = -1;
	uint16_t ef;
	uint64_t missed = 0;
	for (; ; p++)
	{
		const unsigned char* pkt = pcap_next (pc, &h);
		if (pkt == NULL)
			break;
		if (sf == -1)
		{
			sf = (int)frame_seq ((fpga_pkt*)pkt);
		}
		else
		{
			missed += (uint64_t) (
				(uint16_t)(frame_seq ((fpga_pkt*)pkt) - ef) - 1 );
		}
		ef = frame_seq ((fpga_pkt*)pkt);
		pkt_pretty_print ((fpga_pkt*)pkt, stdout);
		printf ("\n");
		fpga_perror (stdout, is_valid ((fpga_pkt*)pkt));
		printf ("\n");
		// dump_pkt (pkt);
	}
	printf ("\n----------\n"
		"Total number of packets: %lu\n"
		"Missed packets:          %lu\n"
		"Start frame:             %d\n"
		"End frame:               %hu\n", p, missed, sf, ef);

	pcap_close (pc);
	return 0;
}
