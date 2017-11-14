.DEFAULT: all
.PHONY: all clean fullclean install

# PROJ_ROOT is set by Makefiles that include this one
LIB_ROOT := $(PROJ_ROOT)/lib
BIN_ROOT := $(PROJ_ROOT)/bin
LIB_SRC := $(PROJ_ROOT)/src/lib
BIN_SRC := $(PROJ_ROOT)/src/bin
CPATH := $(PROJ_ROOT)/include

ext_libs := zmq czmq
bins := $(patsubst %.c,%,$(notdir $(wildcard $(BIN_SRC)/*.c)))
test_bins := $(patsubst %.c,%,$(notdir $(wildcard $(BIN_SRC)/test/*.c)))
libs := $(patsubst %.c,%,$(notdir $(wildcard $(LIB_SRC)/*.c)))
so := $(libs:%=lib%.so)

CC := gcc
# should we filter CFLAGS or allow all?
CFLAGS := -I$(CPATH) -Wall -Wextra -Wno-unused $(CFLAGS)
LDFLAGS := -L$(LIB_ROOT)
