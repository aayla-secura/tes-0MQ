# PROGS := $(patsubst %.c,%,$(wildcard $(BIN_SRC)/*.c))
PROGS := $(SBIN_SRC)/tesd $(BIN_SRC)/tesc

all-progs: $(PROGS)

install-progs: install-prog-tesd install-prog-tesc

install-prog-tesd: | $(SBIN_DEST)
	install $(SBIN_SRC)/tesd $(SBIN_DEST)/tesd

install-prog-tesc: | $(BIN_DEST)
	install $(BIN_SRC)/tesc $(BIN_DEST)/tesc

$(SBIN_DEST):
	install -d $(SBIN_DEST)

$(BIN_DEST):
	install -d $(BIN_DEST)

clean-progs:
	rm -f $(PROGS:%=%.o) $(PROGS)

fullclean-progs: clean-progs
	rm -f $(PROGS:$(BIN_SRC)=$(BIN_DEST))

$(SBIN_SRC)/tesd: %: %.o
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(EXT_LIBS:%=-l% ) $(LIBS:%=-l% ) -o $@

$(BIN_SRC)/tesc: %: %.o
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(EXT_LIBS:%=-l% ) -o $@

.PHONY:: all-progs clean-progs fullclean-progs install-progs $(PROGS:$(BIN_SRC)/=install-prog-)
