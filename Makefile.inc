.DEFAULT: all
.PHONY: all clean fullclean install

PREFIX ?= /usr/local
LIB_ROOT := $(PREFIX)/lib
BIN_ROOT := $(PREFIX)/bin
SBIN_ROOT := $(PREFIX)/sbin

# PROJ_ROOT is set by Makefiles that include this one
LIB_SRC := $(PROJ_ROOT)/src/lib
BIN_SRC := $(PROJ_ROOT)/src/bin
CPATH := $(PROJ_ROOT)/include
HEADERS := $(wildcard $(CPATH)/*.h $(CPATH)/net/*.h)

ext_libs := zmq czmq
bins := $(patsubst %.c,%,$(notdir $(wildcard $(BIN_SRC)/*.c)))
test_bins := $(patsubst %.c,%,$(notdir $(wildcard $(BIN_SRC)/test/*.c)))
libs := $(patsubst %.c,%,$(notdir $(wildcard $(LIB_SRC)/*.c)))
so := $(libs:%=lib%.so)

CC := gcc
# should we filter CFLAGS or allow all?
CFLAGS := -I$(CPATH) -Wall -Wextra -Wno-unused-parameter -Wno-unused-function $(CFLAGS)
LDFLAGS := -L$(LIB_ROOT)
