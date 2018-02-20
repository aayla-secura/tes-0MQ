#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <errno.h>

#ifdef linux
#  define ifr_index ifr_ifindex
#  define sockaddr_dl sockaddr_ll
#  define sdl_family sll_family 
#  define sdl_index sll_ifindex
#  define IFNAME "eth0"
#else
#  define IFNAME "igb1"
#endif

#define PROMISC

int
main (void)
{
	int rc;

	/* A socket is needed for ioctl */
	int sock = socket (AF_INET, SOCK_DGRAM, htons (IPPROTO_IP));
	if (sock == -1)
	{
		perror ("Could not create a raw socket");
		switch (errno)
		{
			case EPROTONOSUPPORT:
				puts ("EPROTONOSUPPORT");
				break;
			case EAFNOSUPPORT:
				puts ("EAFNOSUPPORT");
				break;
			case EPROTOTYPE:
				puts ("EPROTOTYPE");
				break;
			default:
				puts ("Other");
				break;
		}
		return -1;
	}

	/* Retrieve the index of the interface */
	struct ifreq ifr = {0};
	strncpy (ifr.ifr_name, IFNAME, IFNAMSIZ);
	rc = ioctl (sock, SIOCGIFINDEX, &ifr);
	if (rc == -1)
	{
		perror ("Could not get the interface's index");
		return -1;
	}
	printf ("Interface %s has index %d\n", IFNAME, ifr.ifr_index);

	/* Bring the interface up */
	rc = ioctl (sock, SIOCGIFFLAGS, &ifr);
	if (rc == -1)
	{
		perror ("Could not get the interface's state");
		return -1;
	}
	if (! (ifr.ifr_flags & IFF_UP))
	{
		ifr.ifr_flags = IFF_UP;
		rc = ioctl (sock, SIOCSIFFLAGS, &ifr);
		if (rc == -1)
		{
			perror ("Could not bring the interface up");
			return -1;
		}
		/* check */
		rc = ioctl (sock, SIOCGIFFLAGS, &ifr);
		if (rc == -1)
		{
			perror ("Could not get the interface's state");
			return -1;
		}
		if (! (ifr.ifr_flags & IFF_UP))
		{
			fputs ("Could not bring the interface up\n", stderr);
			return -1;
		}
	}
	puts ("Interface is up");

#ifdef PROMISC
	/* Put the interface in promiscuous mode */
	if (! (ifr.ifr_flags & IFF_PROMISC))
	{
#ifdef linux
		ifr.ifr_flags |= IFF_PROMISC;
#else
		ifr.ifr_flags &= ~IFF_PROMISC;
		ifr.ifr_flagshigh |= IFF_PPROMISC >> 16;
#endif
		rc = ioctl (sock, SIOCSIFFLAGS, &ifr);
		if (rc == -1)
		{
			perror ("Could not put the interface in promiscuous mode");
			return -1;
		}
		/* check */
		rc = ioctl (sock, SIOCGIFFLAGS, &ifr);
		if (rc == -1)
		{
			perror ("Could not get the interface's state");
			return -1;
		}
		if (! (ifr.ifr_flags & IFF_PROMISC))
		{
			fputs ("Could not put the interface in promiscuous mode\n", stderr);
			return -1;
		}
	}
	puts ("Interface is in promiscuous mode");
#endif /* PROMISC */

	puts ("Done");
	return 0;
}
