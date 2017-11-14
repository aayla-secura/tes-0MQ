TEST_PROGS := $(patsubst %.c,%,$(wildcard $(TESTBIN_SRC)/*.c))

all-testprogs: $(TEST_PROGS)

clean-testprogs:
	rm -f $(TEST_PROGS:%=%.o)

fullclean-testprogs: clean-testprogs
	rm -f $(TEST_PROGS)

$(filter daemon,$(TEST_PROGS)): %: %.o
	$(CC) $(CFLAGS) $(LDFLAGS) $< -ldaemon -o $@

$(filter thread,$(TEST_PROGS)): %: %.o
	$(CC) $(CFLAGS) $(LDFLAGS) $< -lpthread -o $@

$(filter pcap,$(TEST_PROGS)): %: %.o
	$(CC) $(CFLAGS) $(LDFLAGS) $< -lpcap -o $@

.PHONY:: all-testprogs clean-testprogs fullclean-testprogs
