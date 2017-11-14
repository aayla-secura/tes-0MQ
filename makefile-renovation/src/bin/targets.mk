# PROGS := $(patsubst %.c,%,$(wildcard $(BIN_SRC)/*.c))
PROGS := $(BIN_SRC)/server $(BIN_SRC)/client

all-progs: $(PROGS)

install-progs: $(PROGS:$(BIN_SRC)/=install-prog-)

install-prog-%: | $(BIN_DEST)
	install $(BIN_SRC)/$* $(BIN_DEST)/$*

$(BIN_DEST):
	install -d $(BIN_DEST)

clean-progs:
	rm -f $(PROGS:%=%.o) $(PROGS)

fullclean-progs: clean-progs
	rm -f $(PROGS:$(BIN_SRC)=$(BIN_DEST))

$(BIN_SRC)/server: %: %.o
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(EXT_LIBS:%=-l% ) $(LIBS:%=-l% ) -o $@

$(BIN_SRC)/client: %: %.o
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(EXT_LIBS:%=-l% ) -o $@

.PHONY:: all-progs clean-progs fullclean-progs install-progs $(PROGS:$(BIN_SRC)/=install-prog-)
