PROJ_ROOT := ../../..
include $(PROJ_ROOT)/Makefile.inc

all: $(test_bins)

clean:
	# this is not one of make's implicit rules, good
	rm -f *.o $(test_bins)

%.o: %.c
	# this is not one of make's implicit rules, good
	$(CC) $(CFLAGS) -c $< -o $@

%: %.c

# default, take LDLIBS from environment
%: %.o
	# this is not one of make's implicit rules, good
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

test_daemon: test_daemon.o
	# this is not one of make's implicit rules, good
	$(CC) $(CFLAGS) $(LDFLAGS) $< -ldaemon -o $@

%threaded: %threaded.o
	# this is not one of make's implicit rules, good
	$(CC) $(CFLAGS) $(LDFLAGS) $< -lpthread -o $@

%pcap_rx: %pcap_rx.o
	# this is not one of make's implicit rules, good
	$(CC) $(CFLAGS) $(LDFLAGS) $< -lpcap -o $@

%pcap_tx: %pcap_tx.o
	# this is not one of make's implicit rules, good
	$(CC) $(CFLAGS) $(LDFLAGS) $< -lpcap -o $@
