PREFIX ?= /usr/local
LIB_DEST := $(PREFIX)/lib
BIN_DEST := $(PREFIX)/bin
SBIN_DEST := $(PREFIX)/sbin

# PROJ_ROOT is set by Makefiles that include this one
CPATH := $(PROJ_ROOT)/include
LIB_SRC := $(PROJ_ROOT)/src/lib
BIN_SRC := $(PROJ_ROOT)/src/bin
TESTBIN_SRC := $(PROJ_ROOT)/src/bin/test
HEADERS := $(wildcard $(CPATH)/*.h $(CPATH)/net/*.h)

EXT_LIBS := zmq czmq

CC := gcc
# should we filter CFLAGS or allow all?
CFLAGS := -fPIC -I$(CPATH) -Wall -Wextra -Wno-unused $(CFLAGS)
LDFLAGS := -L$(LIB_DEST) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%: %.o
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@

.DEFAULT_GOAL := all
.PHONY:: all clean fullclean install
