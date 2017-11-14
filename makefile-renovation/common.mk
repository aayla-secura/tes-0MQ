# TO DO: depend on headers
# PROJ_ROOT is set by Makefiles that include this one
CPATH := $(PROJ_ROOT)/include
LIB_DEST := $(PROJ_ROOT)/lib
BIN_DEST := $(PROJ_ROOT)/bin
LIB_SRC := $(PROJ_ROOT)/src/lib
BIN_SRC := $(PROJ_ROOT)/src/bin
TESTBIN_SRC := $(PROJ_ROOT)/src/bin/test

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
