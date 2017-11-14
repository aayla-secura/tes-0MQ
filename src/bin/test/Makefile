PROJ_ROOT := ../../..
include $(PROJ_ROOT)/Makefile.inc

all: $(test_bins)

clean:
	rm -f *.o

fullclean:
	rm -f *.o $(test_bins)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%: %.c

# default, take LDLIBS from environment
%: %.o
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

test_daemon: test_daemon.o
	$(CC) $(CFLAGS) $(LDFLAGS) $< -ldaemon -o $@

%threaded: %threaded.o
	$(CC) $(CFLAGS) $(LDFLAGS) $< -lpthread -o $@

%pcap_rx: %pcap_rx.o
	$(CC) $(CFLAGS) $(LDFLAGS) $< -lpcap -o $@

%pcap_tx: %pcap_tx.o
	$(CC) $(CFLAGS) $(LDFLAGS) $< -lpcap -o $@
