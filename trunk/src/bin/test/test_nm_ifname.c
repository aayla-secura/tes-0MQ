#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <net/if.h> /* IFNAMSIZ */

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>

#ifndef NMIF
#define NMIF "vale1:tes"
#endif

#define ERROR(...) fprintf (stdout, __VA_ARGS__)
#define DEBUG(...) fprintf (stderr, __VA_ARGS__)
#define INFO(...)  fprintf (stdout, __VA_ARGS__)

int
main (void)
{
	/* Open the interface */
	struct nm_desc* nmd = nm_open(NMIF, NULL, 0, NULL);
	if (nmd == NULL)
	{
		perror ("Could not open interface");
		return -1;
	}

	INFO (
		"name: %s (%s)\n"
		"ringid: %hu, flags: %u, cmd: %hu\n"
		"extra rings: %hu, extra buffers: %u\n"
		"done_mmap: %d\n"
		"rx rings: %hu, rx slots: %u\n"
		"tx rings: %hu, tx slots: %u\n"
		"first rx: %hu, last rx: %hu\n"
		"first tx: %hu, last tx: %hu\n"
		"snaplen: %d\npromisc: %d\n",
		/* nifp->ni_name contains the true name as opened, e.g. if the
		 * interface is a persistent vale port, it will contain
		 * vale*:<port> even if nm_open was passed netmap:<port> */
		nmd->nifp->ni_name,
		/* req.nr_name contains the name of the interface passed to
		 * nm_open minus the ring specification and minus optional
		 * netmap: prefix, even if interface is a vale port */
		nmd->req.nr_name,
		nmd->req.nr_ringid,
		nmd->req.nr_flags,
		nmd->req.nr_cmd,
		nmd->req.nr_arg1,
		nmd->req.nr_arg3,
		nmd->done_mmap,
		nmd->req.nr_rx_rings,
		nmd->req.nr_rx_slots,
		nmd->req.nr_tx_rings,
		nmd->req.nr_tx_slots,
		nmd->first_rx_ring,
		nmd->last_rx_ring,
		nmd->first_tx_ring,
		nmd->last_tx_ring,
		nmd->snaplen,
		nmd->promisc
		);

	nm_close (nmd);
	return 0;
}
