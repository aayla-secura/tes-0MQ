.DEFAULT: all
.PHONY: all clean install

# PROJ_ROOT is set by Makefiles that include this one
LIB_ROOT := $(PROJ_ROOT)/lib
BIN_ROOT := $(PROJ_ROOT)/bin
LIB_SRC := $(PROJ_ROOT)/src/lib
BIN_SRC := $(PROJ_ROOT)/src/bin
CPATH := $(PROJ_ROOT)/include

ext_libs := zmq czmq pthread
bins := $(patsubst %.c,%,$(notdir $(wildcard $(BIN_SRC)/*.c)))
libs := $(patsubst %.c,%,$(notdir $(wildcard $(LIB_SRC)/*.c)))
so := $(libs:%=lib%.so)

CC := gcc
CFLAGS := -Wno-unused -Wall -Wextra -O1 -I$(CPATH) $(filter -O0 -O2 -DSYSLOG -DVERBOSE -DFPGA_DEBUG,$(CFLAGS))
LDLIBS := $(ext_libs:%=-l% )
LDFLAGS := -L$(LIB_ROOT)
