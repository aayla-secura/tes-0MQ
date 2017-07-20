#define NETMAP_WITH_LIBS

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h> /* offsetof */
#include <assert.h> /* offsetof */
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <errno.h>
#include <poll.h>
#include <netinet/ether.h>
#include <net/netmap_user.h>

#define NM_IFNAME "vale:fpga"
#define FPGA_HDR_LEN 24 /* includes the 16 byte ethernet header */
#define MCA_HDR_LEN  40
#define TICK_HDR_LEN 24
#define PEAK_HDR_LEN  8
#define PEAK_LEN      8
#define PULSE_LEN     8
#define PULSE_HDR_LEN 8 + PULSE_LEN
#define AREA_HDR_LEN  8
#define TRACE_HDR_LEN 8
#define TRACE_FULL_HDR_LEN TRACE_HDR_LEN + PULSE_LEN
#define DOTP_LEN      8
#define SMPL_LEN      2
#define BIN_LEN       4
#define MCA_FL_LEN    4
#define EVT_FL_LEN    2
#define TICK_FL_LEN   2
#define TRACE_FL_LEN  2
#define MAX_FRAMES  184 /* max body size in multiples of 8 bytes (will just fit
			 * along with the 24-byte header in a 1500-byte packet */
#define FPGA_PKT_LEN FPGA_HDR_LEN + MAX_FRAMES * 8
#define MAX_PKTS 1024 /* keep pointers to packets to be freed by the signal handler */

#define SRC_HW_ADDR "ff:ff:ff:ff:ff:ff"
#define DST_HW_ADDR "ff:ff:ff:ff:ff:ff"

#define ETH_TYPE_EVT 0x88B5
#define ETH_TYPE_MCA 0x88B6
#define EVT_TICK_TYPE    0x0002
#define EVT_PEAK_TYPE    0x0000
#define EVT_PULSE_TYPE   0x0004
#define EVT_AREA_TYPE    0x0008
#define EVT_TR_SGL_TYPE  0x000c
#define EVT_TR_AVG_TYPE  0x010c
#define EVT_TR_DP_TYPE   0x020c
#define EVT_TR_DPTR_TYPE 0x030c

#define ERROR(...) fprintf (stdout, __VA_ARGS__)
#define PERROR(msg) perror (msg)
#define DEBUG(...) fprintf (stderr, __VA_ARGS__)
#define INFO(...) fprintf (stdout, __VA_ARGS__)

// struct ether_header, struct ether_addr
// ether_aton_r, ether_ntoa_r, ntohl, ntohs, htonl, htons
// inet_aton, inet_ntoa, inet_addr, inet_network, inet_pton, inet_ntop
// getifaddrs
// IPPROTO_UDP, ETH_ALEN, INADDR_BROADCAST, ETHERTYPE_IP, ETHER_*_LEN

// from pkt-gen:
// system_ncpus, dump_payload, source_hwaddr, checksum

typedef struct __fpga_pkt fpga_pkt;

struct mca_flags
{
#if __BYTE_ORDER == __LITTLE_ENDIAN
	u_int8_t C :  3;
	u_int8_t N :  5;
	u_int8_t T :  4;
	u_int8_t V :  4;
	u_int8_t Q :  4;
	u_int16_t  : 12; /* reserved */
#else
	u_int16_t  : 12; /* reserved */
	u_int8_t Q :  4;
	u_int8_t V :  4;
	u_int8_t T :  4;
	u_int8_t N :  5;
	u_int8_t C :  3;
#endif
};

struct event_flags
{
#if __BYTE_ORDER == __LITTLE_ENDIAN
	u_int8_t N  : 1;
	u_int8_t T  : 1;
	u_int8_t PT : 2;
	u_int8_t HT : 2;
	u_int8_t TT : 2;
	u_int8_t CH : 3;
	u_int8_t O  : 1;
	u_int8_t PC : 4;
#else
	u_int8_t PC : 4;
	u_int8_t O  : 1;
	u_int8_t CH : 3;
	u_int8_t TT : 2;
	u_int8_t HT : 2;
	u_int8_t PT : 2;
	u_int8_t T  : 1;
	u_int8_t N  : 1;
#endif
};

struct tick_flags
{
#if __BYTE_ORDER == __LITTLE_ENDIAN
	u_int8_t N  : 1;
	u_int8_t T  : 1;
	u_int8_t    : 6; /* reserved */
	u_int8_t TL : 1;
	u_int8_t EL : 1;
	u_int8_t MF : 1;
	u_int8_t    : 5; /* reserved */
#else
	u_int8_t    : 5; /* reserved */
	u_int8_t MF : 1;
	u_int8_t EL : 1;
	u_int8_t TL : 1;
	u_int8_t    : 6; /* reserved */
	u_int8_t T  : 1;
	u_int8_t N  : 1;
#endif
};

struct trace_flags
{
#if __BYTE_ORDER == __LITTLE_ENDIAN
	u_int8_t OFF : 4;
	u_int8_t TS  : 2;
	u_int8_t TT  : 2;
	u_int8_t STR : 5;
	u_int8_t MP  : 1;
	u_int8_t MH  : 1;
	u_int8_t     : 1; /* reserved */
#else
	u_int8_t     : 1; /* reserved */
	u_int8_t MH  : 1;
	u_int8_t MP  : 1;
	u_int8_t STR : 5;
	u_int8_t TT  : 2;
	u_int8_t TS  : 2;
	u_int8_t OFF : 4;
#endif
};

struct __mca_header
{
	u_int16_t size;
	u_int16_t last_bin;
	u_int32_t lowest_value;
	u_int16_t : 16; /* reserved */
	u_int16_t most_frequent;
	struct mca_flags flags;
	u_int64_t total;
	u_int64_t start_time;
	u_int64_t stop_time;
};

struct __tick_header
{
	u_int32_t period;
	struct event_flags flags;
	u_int16_t toff; /* time since last event */
	u_int64_t ts;   /* timestamp */
	u_int8_t ovrfl;
	u_int8_t err;
	u_int8_t cfd;
	u_int8_t : 8;   /* reserved */
	u_int32_t lost;
};

struct __peak_header
{
	u_int16_t height;
	u_int16_t rise_time;
	struct event_flags flags;
	u_int16_t toff;
};

struct __peak
{
	u_int16_t height;
	u_int16_t rise_time;
	u_int16_t minimum;
	u_int16_t toff;
};

struct __pulse
{
	u_int32_t area;
	u_int16_t length;
	u_int16_t toffset;
};

struct __pulse_header
{
	u_int16_t size;
	u_int16_t : 16; /* reserved */
	struct event_flags flags;
	u_int16_t toff;
	struct __pulse pulse;
};

struct __area_header
{
	u_int32_t area;
	struct event_flags flags;
	u_int16_t toff;
};

struct __trace_header
{
	u_int16_t size;
	struct trace_flags tr_flags;
	struct event_flags flags;
	u_int16_t toff;
};

struct __trace_full_header
{
	struct __trace_header trace;
	struct __pulse pulse;
};

struct __dot_prod
{
	u_int16_t : 16; /* reserved */
	u_int64_t dot_prod : 48;
} __attribute__ ((__packed__));

struct __fpga_pkt
{
	struct
	{
		struct ether_header eth_hdr; /*packed, 14 bytes*/
		u_int16_t length;	     /*length of packet - 16*/
	};
	struct
	{
		u_int16_t frame_seq;
		u_int16_t proto_seq;
		u_int16_t evt_size; /* undefined for MCA frames */
		u_int16_t evt_type; /* undefined for MCA frames */
	} fpga_hdr;
	u_int64_t body[MAX_FRAMES];
};

/* ------------------------------------------------------------------------- */

static struct
{
	struct timeval time_start;
	struct timeval time_end;
	struct timeval time_diff;
	struct nm_desc* nmd;
	struct {
		fpga_pkt* slots[MAX_PKTS];
		int last;       /* highest allocated index */
		int first_free; /* lowest unallocated index */
	} pkts;
	u_int loop;
} gobj = { .pkts.last = -1 };

static fpga_pkt* new_fpga_pkt (void)
{
	fpga_pkt* pkt = malloc (FPGA_PKT_LEN);
	if (pkt == NULL)
	{
		errno = ENOMEM;
		raise (SIGTERM);
	}
	memset (pkt, 0, FPGA_PKT_LEN);

	struct ether_addr* mac_addr = ether_aton (DST_HW_ADDR);
	memcpy (&pkt->eth_hdr.ether_dhost, mac_addr, ETH_ALEN);
	mac_addr = ether_aton (SRC_HW_ADDR);
	memcpy (&pkt->eth_hdr.ether_shost, mac_addr, ETH_ALEN);
	pkt->fpga_hdr.frame_seq = gobj.nmd->st.ps_recv;
	pkt->length = FPGA_HDR_LEN;

	/* store a global pointer */
	gobj.pkts.slots[gobj.pkts.first_free] = pkt;
	assert (gobj.pkts.first_free != gobj.pkts.last);
	INFO ("Creating packet #%d\n", gobj.pkts.first_free);
	if (gobj.pkts.first_free < gobj.pkts.last)
	{
		/* there were holes in the array, find the next freed slot */
		int p;
		for (p = gobj.pkts.first_free + 1; p <= gobj.pkts.last; p++)
		{
			if (gobj.pkts.slots[p] == NULL)
				break;
		}
		gobj.pkts.first_free = p;
	}
	else
	{
		/* simply append */
		gobj.pkts.first_free++;
		gobj.pkts.last++;
	}

	return pkt;
}

static fpga_pkt* new_mca_pkt (int seq, int num_bins,
			      int num_all_bins,
			      struct mca_flags* flags)
{
	fpga_pkt* pkt = new_fpga_pkt ();
	pkt->eth_hdr.ether_type = ETH_TYPE_MCA;
	pkt->length += num_bins * BIN_LEN;

	pkt->fpga_hdr.proto_seq = seq;
	if (seq == 0)
	{
		pkt->length += MCA_HDR_LEN;
		struct __mca_header* mh = (struct __mca_header*) &pkt->body;
		mh->size = MCA_HDR_LEN + num_all_bins * BIN_LEN;
		mh->last_bin = num_all_bins - 1;
		mh->lowest_value = (u_int32_t) random ();
		/* mh->most_frequent =  */
		memcpy (&mh->flags, flags, MCA_FL_LEN);
		mh->total = (u_int64_t) mh->lowest_value * num_all_bins;
		mh->start_time = (u_int64_t) random ();
		mh->stop_time = mh->start_time + (u_int32_t) random ();
	}

	return pkt;
}

static fpga_pkt* new_tick_pkt (struct tick_flags* flags)
{
	fpga_pkt* pkt = new_fpga_pkt ();
	pkt->eth_hdr.ether_type = ETH_TYPE_EVT;
	pkt->length += TICK_HDR_LEN;
	pkt->fpga_hdr.evt_size = 3;
	pkt->fpga_hdr.evt_type = EVT_TICK_TYPE;

	struct __tick_header* th = (struct __tick_header*) &pkt->body;
	th->period = (u_int32_t) random ();
	memcpy (&th->flags, flags, TICK_FL_LEN);
	th->toff = (u_int16_t) random ();
	th->ts = random ();
	th->ovrfl = (u_int8_t) random ();
	th->err = (u_int8_t) random ();
	th->cfd = (u_int8_t) random ();
	th->lost = (u_int32_t) random ();

	return pkt;
}

static fpga_pkt* new_peak_pkt (struct event_flags* flags)
{
	fpga_pkt* pkt = new_fpga_pkt ();
	pkt->eth_hdr.ether_type = ETH_TYPE_EVT;
	pkt->length += PEAK_HDR_LEN;
	pkt->fpga_hdr.evt_size = 1;
	pkt->fpga_hdr.evt_type = EVT_PEAK_TYPE;

	struct __peak_header* ph = (struct __peak_header*) &pkt->body;
	ph->height = (u_int16_t) random ();
	ph->rise_time = (u_int16_t) random ();
	memcpy (&ph->flags, flags, EVT_FL_LEN);
	ph->toff = (u_int16_t) random ();

	return pkt;
}

static fpga_pkt* new_pulse_pkt (int num_peaks, struct event_flags* flags)
{
	fpga_pkt* pkt = new_fpga_pkt ();
	pkt->eth_hdr.ether_type = ETH_TYPE_EVT;
	pkt->length += PULSE_HDR_LEN + num_peaks * PEAK_LEN;
	pkt->fpga_hdr.evt_size = num_peaks; /* is it?? */
	pkt->fpga_hdr.evt_type = EVT_PULSE_TYPE;

	struct __pulse_header* ph = (struct __pulse_header*) &pkt->body;
	ph->size = (u_int16_t) random ();
	memcpy (&ph->flags, flags, EVT_FL_LEN);
	ph->toff = (u_int16_t) random ();
	ph->pulse.area = (u_int32_t) random ();
	ph->pulse.length = (u_int16_t) random ();
	ph->pulse.toffset = (u_int16_t) random ();

	/* peaks... */

	return pkt;
}

static fpga_pkt* new_area_pkt (struct event_flags* flags)
{
	fpga_pkt* pkt = new_fpga_pkt ();
	pkt->eth_hdr.ether_type = ETH_TYPE_EVT;
	pkt->length += AREA_HDR_LEN;
	pkt->fpga_hdr.evt_size = 1;
	pkt->fpga_hdr.evt_type = EVT_AREA_TYPE;

	struct __area_header* ah = (struct __area_header*) &pkt->body;
	ah->area = (u_int32_t) random ();
	memcpy (&ah->flags, flags, EVT_FL_LEN);
	ah->toff = (u_int16_t) random ();

	return pkt;
}

static fpga_pkt* new_trace_single_pkt (int num_peaks,
				       int num_samples,
				       struct trace_flags* tr_flags,
				       struct event_flags* flags)
{
	fpga_pkt* pkt = new_fpga_pkt ();
	pkt->eth_hdr.ether_type = ETH_TYPE_EVT;
	pkt->length += TRACE_FULL_HDR_LEN + num_peaks * PEAK_LEN + num_samples * SMPL_LEN;
	pkt->fpga_hdr.evt_size = 1;
	pkt->fpga_hdr.evt_type = EVT_TR_SGL_TYPE;

	struct __trace_full_header* th = (struct __trace_full_header*) &pkt->body;
	th->trace.size = (u_int16_t) random ();
	memcpy (&th->trace.tr_flags, tr_flags, TRACE_FL_LEN);
	memcpy (&th->trace.flags, flags, EVT_FL_LEN);
	th->trace.toff = (u_int16_t) random ();
	th->pulse.area = (u_int32_t) random ();
	th->pulse.length = (u_int16_t) random ();
	th->pulse.toffset = (u_int16_t) random ();

	/* peaks */

	/* samples */

	return pkt;
}

static fpga_pkt* new_trace_avg_pkt (int num_samples,
				    struct trace_flags* tr_flags,
				    struct event_flags* flags)
{
	fpga_pkt* pkt = new_fpga_pkt ();
	pkt->eth_hdr.ether_type = ETH_TYPE_EVT;

	return pkt;
}

static fpga_pkt* new_trace_dp_pkt (int num_peaks,
				   struct trace_flags* tr_flags,
				   struct event_flags* flags)
{
	fpga_pkt* pkt = new_fpga_pkt ();
	pkt->eth_hdr.ether_type = ETH_TYPE_EVT;
	pkt->length += TRACE_FULL_HDR_LEN + num_peaks * PEAK_LEN;
	pkt->fpga_hdr.evt_size = 1;
	pkt->fpga_hdr.evt_type = EVT_TR_DP_TYPE;

	struct __trace_full_header* th = (struct __trace_full_header*) &pkt->body;
	th->trace.size = (u_int16_t) random ();
	memcpy (&th->trace.tr_flags, tr_flags, TRACE_FL_LEN);
	memcpy (&th->trace.flags, flags, EVT_FL_LEN);
	th->trace.toff = (u_int16_t) random ();
	th->pulse.area = (u_int32_t) random ();
	th->pulse.length = (u_int16_t) random ();
	th->pulse.toffset = (u_int16_t) random ();

	/* peaks */

	struct __dot_prod* dp = (struct __dot_prod*)(
		(u_char*) pkt + pkt->length );
	dp->dot_prod = random (); /* how to cast to 48 bit */
	pkt->length += DOTP_LEN;

	return pkt;
}

static fpga_pkt* new_trace_dptr_pkt (int num_peaks,
				     int num_samples,
				     struct trace_flags* tr_flags,
				     struct event_flags* flags)
{
	fpga_pkt* pkt = new_fpga_pkt ();
	pkt->eth_hdr.ether_type = ETH_TYPE_EVT;

	return pkt;
}

static void destroy_pkt (int id)
{
	assert (id >= 0);
	assert (id <= gobj.pkts.last);
	fpga_pkt* pkt = gobj.pkts.slots[id];
	if (pkt)
	{
		INFO ("Destroying packet #%d\n", id);
		free (pkt);
		gobj.pkts.slots[id] = NULL;
		if (id == gobj.pkts.last)
			gobj.pkts.last--;
		if (id < gobj.pkts.first_free)
			gobj.pkts.first_free = id;

	}
}

static void dump_pkt (fpga_pkt* _pkt)
{
	u_int16_t len = _pkt->length;
	const u_char* pkt = (const u_char*)_pkt;
	char buf[48];
	memset (buf, 0, 48);

	for (int f = 0; f < len; f += 8) {
		sprintf (buf, "%04x: ", f);

		/* hexdump */
		for (int b = 0; b < 8 && b+f < len; b++)
			sprintf (buf + 6 + b*3, "%02x ",
				(u_int8_t)(pkt[b+f]));

		/* ASCII dump */
		for (int b = 0; b < 8 && b+f < len; b++)
			sprintf (buf + 6 + b + 24, "%c",
				isprint (pkt[b+f]) ? pkt[b+f] : '.');

		printf ("%s\n", buf);
	}
	puts ("");
}

static void print_desc_info (void)
{
	INFO (
		"ringid: %hu, flags: %u, cmd: %hu\n"
		"extra rings: %hu, extra buffers: %u\n"
		"done_mmap: %d\n"
		"rx rings: %d, rx slots: %d\n"
		"tx rings: %d, tx slots: %d\n"
		"first rx: %hu, last rx: %hu\n"
		"first tx: %hu, last tx: %hu\n"
		"snaplen: %d\npromisc: %d\n",
		gobj.nmd->req.nr_ringid,
		gobj.nmd->req.nr_flags,
		gobj.nmd->req.nr_cmd,
		gobj.nmd->req.nr_arg1,
		gobj.nmd->req.nr_arg3,
		gobj.nmd->done_mmap,
		gobj.nmd->req.nr_rx_rings,
		gobj.nmd->req.nr_rx_slots,
		gobj.nmd->req.nr_tx_rings,
		gobj.nmd->req.nr_tx_slots,
		gobj.nmd->first_rx_ring,
		gobj.nmd->last_rx_ring,
		gobj.nmd->first_tx_ring,
		gobj.nmd->last_tx_ring,
		gobj.nmd->snaplen,
		gobj.nmd->promisc
		);
}

static void print_stats (void)
{
	if (gobj.time_start.tv_sec == 0)
		return;

	timersub (&gobj.time_end, &gobj.time_start, &gobj.time_diff);
	double tdiff = gobj.time_diff.tv_sec + 1e-6 * gobj.time_diff.tv_usec;
	
	INFO (
		"looped:\t\t\t%u\n"
		"sent:\t\t\t%u\n"
		/* "dropped by OS:\t%u\n" */
		/* "dropped by IF:\t%u\n" */
		"avg pkts per loop:\t%u\n"
		"avg bandwidth:\t\t%.3Le pps\n",
		gobj.loop,
		gobj.nmd->st.ps_recv,
		/* gobj.nmd->st.ps_drop, */
		/* gobj.nmd->st.ps_ifdrop, */
		(gobj.loop > 0) ? gobj.nmd->st.ps_recv / gobj.loop : 0,
		(long double) gobj.nmd->st.ps_recv / tdiff
		);
}

static void cleanup (int sig)
{
	INFO ("Received %d\n", sig);

	int rc = EXIT_SUCCESS;
	if (errno)
	{
		PERROR ("");
		rc = EXIT_FAILURE;
	}

	if (gobj.nmd)
	{
		gettimeofday (&gobj.time_end, NULL);
		print_stats ();
		nm_close (gobj.nmd);
	}

	for (int p = 0; p <= gobj.pkts.last; p++)
		destroy_pkt (p);

	exit (rc);
}

int main (void)
{
	/* Debugging */
	assert (sizeof (fpga_pkt) == FPGA_PKT_LEN);
	assert (offsetof (fpga_pkt, body) == FPGA_HDR_LEN);
	assert (sizeof (struct __mca_header) == MCA_HDR_LEN);
	assert (sizeof (struct __tick_header) == TICK_HDR_LEN);
	assert (sizeof (struct __peak_header) == PEAK_HDR_LEN);
	assert (sizeof (struct __peak) == PEAK_LEN);
	assert (sizeof (struct __pulse_header) == PULSE_HDR_LEN);
	assert (sizeof (struct __pulse) == PULSE_LEN);
	assert (sizeof (struct __area_header) == AREA_HDR_LEN);
	assert (sizeof (struct __trace_header) == TRACE_HDR_LEN);
	assert (sizeof (struct __trace_full_header) == TRACE_FULL_HDR_LEN);
	assert (sizeof (struct __dot_prod) == DOTP_LEN);

	srandom ( (unsigned int) random () );
	int rc;

	/* Signal handlers */
	struct sigaction sigact;
	sigact.sa_flags = 0;
	sigact.sa_handler = cleanup;
	sigemptyset (&sigact.sa_mask);
	rc = sigaction (SIGINT, &sigact, NULL);
	rc |= sigaction (SIGTERM, &sigact, NULL);
	if (rc == -1)
	{
		PERROR ("sigaction");
		exit (EXIT_FAILURE);
	}

	/* Open the interface */
	gobj.nmd = nm_open (NM_IFNAME"{1", NULL, 0, 0);
	if (gobj.nmd == NULL) {
		ERROR ("Could not open interface %s\n", NM_IFNAME);
		exit (EXIT_FAILURE);
	}
	print_desc_info ();

#if 0
	fpga_pkt* pkt;
	struct mca_flags m_flags = {0,};
	m_flags.C = 1;
	m_flags.T = 2;
	m_flags.Q = 3;
	pkt = new_mca_pkt (0, 8, 16, &m_flags);
	puts ("\n--- MCA 0 ---");
	dump_pkt (pkt);

	pkt = new_mca_pkt (1, 8, 16, 0);
	puts ("\n--- MCA 1 ---");
	dump_pkt (pkt);

	struct tick_flags t_flags = {0,};
	t_flags.T  = 1;
	t_flags.EL = 1;
	pkt = new_tick_pkt (&t_flags);
	puts ("\n--- Tick ---");
	dump_pkt (pkt);

	struct event_flags evt_flags = {0,};
	evt_flags.T  = 1;
	evt_flags.CH = 5;
	pkt = new_peak_pkt (&evt_flags);
	puts ("\n--- Peak ---");
	dump_pkt (pkt);

	pkt = new_pulse_pkt (3, &evt_flags);
	puts ("\n--- Pulse ---");
	dump_pkt (pkt);

	pkt = new_area_pkt (&evt_flags);
	puts ("\n--- Area ---");
	dump_pkt (pkt);

	struct trace_flags tr_flags = {0,};
	tr_flags.OFF =  2;
	tr_flags.STR = 15;
	tr_flags.MP  =  1;
	pkt = new_trace_single_pkt (2, 5, &tr_flags, &evt_flags);
	puts ("\n--- Trace (single) ---");
	dump_pkt (pkt);

	/* pkt = new_trace_avg_pkt (2, &tr_flags, &evt_flags); */
	/* puts ("\n--- Trace (dot prod) ---"); */
	/* dump_pkt (pkt); */

	pkt = new_trace_dp_pkt (2, &tr_flags, &evt_flags);
	puts ("\n--- Trace (dot prod) ---");
	dump_pkt (pkt);

	/* pkt = new_trace_dptr_pkt (2, &tr_flags, &evt_flags); */
	/* puts ("\n--- Trace (dot prod) ---"); */
	/* dump_pkt (pkt); */

#else
	/* ------------------------------------------------------------ */

	/* Create the packet */
	fpga_pkt* pkt;
	struct mca_flags m_flags = {0,};
	m_flags.C = 1;
	m_flags.T = 2;
	m_flags.Q = 3;
	pkt = new_mca_pkt (0, 358, 358, &m_flags); /* 358 bins + MCA header
						    * fill up MAX_FRAMES */
	puts ("Sending:\n");
	dump_pkt (pkt);

	/* Get the ring (we only use one) */
	assert (gobj.nmd->first_tx_ring == gobj.nmd->last_tx_ring);
	struct netmap_ring* txring = NETMAP_TXRING (
			gobj.nmd->nifp, gobj.nmd->cur_tx_ring);

	/* Start the clock */
	rc = gettimeofday (&gobj.time_start, NULL);
	if (rc == -1)
	{
		PERROR ("gettimeofday");
		exit (EXIT_FAILURE);
	}

	/* Poll */
	struct pollfd pfd;
	pfd.fd = gobj.nmd->fd;
	pfd.events = POLLOUT;
	DEBUG ("Starting poll\n");

	for (gobj.loop = 1, errno = 0 ;; gobj.loop++)
	{
		rc = poll (&pfd, 1, 1000);
		if (rc == -1)
		{
			PERROR ("poll");
			break;
		}
		if (rc == 0)
		{
			INFO ("poll timed out\n");
			continue;
		}

		do
		{
			struct netmap_slot* cur_slot = &txring->slot[ txring->cur ];
			nm_pkt_copy (pkt,
				     NETMAP_BUF (txring, cur_slot->buf_idx),
				     pkt->length);
		
			cur_slot->len = pkt->length;
			txring->head = txring->cur = nm_ring_next(txring, txring->cur);
			gobj.nmd->st.ps_recv++; /* sent, not received */
			if (gobj.nmd->st.ps_recv + 1 == 0)
			{
				errno = EOVERFLOW;
				raise (SIGINT);
			}
		} while ( ! nm_ring_empty (txring) );
	}

#endif
	errno = 0;
	raise (SIGTERM); /*cleanup*/
	return 0; /*never reached, suppress gcc warning*/
}
