PREFIX := /usr/local

LIB_SRC  := $(abspath src/lib)
BIN_SRC  := $(abspath src/bin)
TEST_SRC := $(abspath test-apps)
CPATH    := $(abspath include)
LIB_DEST := $(abspath lib)
BIN_DEST := $(abspath bin)

PROGS      := tesd tesc
LIBS       := $(patsubst %.c,%,$(notdir $(wildcard $(LIB_SRC)/*.c)))
HEADERS    := $(wildcard $(CPATH)/*.h $(CPATH)/net/*.h)
TEST_PROGS := $(patsubst %.c,%,$(notdir $(wildcard $(TEST_SRC)/*.c)))

CC      := gcc
CFLAGS  += -I$(CPATH) -fPIC -Wall -Wextra \
	-Wno-unused-parameter -Wno-unused-function
LDFLAGS += -L$(LIB_DEST)
LDLIBS  := -lzmq -lczmq

all: main test

##################################################

main: $(BIN_DEST)/tesd $(BIN_DEST)/tesc
	@echo
	@echo "Now run 'make install'"

$(BIN_DEST)/tesc: $(BIN_SRC)/tesc.c \
	| $(BIN_DEST)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

$(BIN_DEST)/tesd: $(BIN_SRC)/tesd.c \
	$(LIBS:%=$(LIB_DEST)/lib%.so) $(HEADERS) \
	| $(BIN_DEST)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) $(LIBS:%=-l% ) -o $@

$(LIB_DEST)/lib%.so: $(LIB_SRC)/%.c $(HEADERS) \
	| $(LIB_DEST)
	$(CC) $(CFLAGS) -c $< -o $@
	$(CC) $(CFLAGS) $(LDFLAGS) -shared $< -o $@

$(LIB_DEST) $(BIN_DEST):
	install -d $@

##################################################

test: $(TEST_PROGS:%=$(BIN_DEST)/%) \
	| $(BIN_DEST)

$(BIN_DEST)/%: $(TEST_SRC)/%.c $(HEADERS)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(foreach lib, \
		$(findstring daemon_ng,$*) \
		$(findstring pthread,$*) \
		$(findstring pcap,$*), \
		-l$(lib)) -o $@

##################################################

install: $(BIN_DEST)/tesd $(BIN_DEST)/tesc \
	$(LIBS:%=install-lib-%)
	install -m 755 $(BIN_DEST)/tesd $(PREFIX)/sbin/tesd
	install -m 755 $(BIN_DEST)/tesc $(PREFIX)/bin/tesc
	@echo
	@echo "Now run 'make clean'"

install-lib-%: $(LIB_DEST)/lib%.so
	install -m 755 $< $(PREFIX)/lib/lib$*.so

##################################################

clean:
	rm -f $(LIB_SRC)/*.o $(BIN_SRC)/*.o $(TEST_SRC)/*.o \
		$(BIN_DEST)/* $(LIB_DEST)/*

.DEFAULT_GOAL := main
.PHONY: all main test clean install
