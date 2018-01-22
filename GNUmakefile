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
LDLIBS  := -lzmq -lczmq -lrt
UNAME := $(shell uname -o)

ifeq ($(HDF5LIB),)
ifeq ($(UNAME),FreeBSD)
      HDF5LIB := "-lhdf5"
else ifeq ($(UNAME),GNU/Linux)
      HDF5LIB := "-lhdf5_serial"
else  # unknown OS
      $(error I do not know what the HDF5 library is called on $(UNAME),\
              define the HDF5LIB variable as "-l<lib>".)
endif
endif # end HDF5LIB == ""

all: test main

##################################################

main: $(BIN_DEST)/tesd $(BIN_DEST)/tesc $(HEADERS) \
	| $(BIN_DEST)
	@echo
	@echo "Now run 'make install'"

libs: $(LIBS:%=$(LIB_DEST)/lib%.a) $(HEADERS) \
	| $(LIB_DEST)

$(BIN_DEST)/tesc: $(BIN_SRC)/tesc.c
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BIN_DEST)/tesd: $(BIN_SRC)/tesd.o $(BIN_SRC)/tesd_tasks.o \
	$(LIBS:%=$(LIB_DEST)/lib%.a)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(HDF5LIB) $(LDLIBS) -o $@

$(LIB_DEST)/lib%.a: $(LIB_SRC)/%.o
	ar rcs $@ $^

$(LIB_DEST) $(BIN_DEST):
	install -d $@

##################################################

test: $(TEST_PROGS:%=$(BIN_DEST)/%) $(HEADERS) \
	| $(BIN_DEST)

# name of program should include any needed libraries
$(BIN_DEST)/%: $(TEST_SRC)/%.o $(LIBS:%=$(LIB_DEST)/lib%.a)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ \
		$(foreach lib, \
			$(findstring pthread,$*) \
			$(findstring pcap,$*), \
			-l$(lib)) \
		$(if $(findstring hdf5,$*),$(HDF5LIB)) \
		 -o $@

##################################################

install: $(BIN_DEST)/tesd $(BIN_DEST)/tesc
	install -m 755 $(BIN_DEST)/tesd $(PREFIX)/sbin/tesd
	install -m 755 $(BIN_DEST)/tesc $(PREFIX)/bin/tesc
	@echo
	@echo "Now run 'make clean'"

##################################################

clean:
	rm -f $(LIB_SRC)/*.o $(BIN_SRC)/*.o $(TEST_SRC)/*.o \
		$(BIN_DEST)/* $(LIB_DEST)/*

.DEFAULT_GOAL := main
.PHONY: all main libs test clean install
