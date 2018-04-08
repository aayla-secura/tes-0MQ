PREFIX := /usr/local

LIB_SRC  := $(abspath src/lib)
BIN_SRC  := $(abspath src/bin)
TEST_SRC := $(abspath tests)
CPATH    := $(abspath include)
LIB_DEST := $(abspath lib)
BIN_DEST := $(abspath bin)

PROGS      := tesd tesc
LIBS       := $(patsubst %.c,%,$(notdir $(wildcard $(LIB_SRC)/*.c)))
HEADERS    := $(wildcard $(CPATH)/*.h $(CPATH)/net/*.h)
TEST_PROGS := $(patsubst %.c,%,$(notdir $(wildcard $(TEST_SRC)/*.c)))
TASKS_OBJ  := $(patsubst %.c,%.o,$(wildcard $(BIN_SRC)/tesd_task_*.c))

CC      := gcc
CFLAGS  += -I$(CPATH) -O1 -Wl,--gc-sections \
           -fdata-sections -ffunction-sections -fPIC \
           -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
LDLIBS  := -lzmq -lczmq -lrt -lpthread
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

##################################################

all: libs tests main

##################################################

tesd: $(BIN_DEST)/tesd

tesc: $(BIN_DEST)/tesc

task_%:
	@touch $(BIN_SRC)/tesd_task_$*.c
	@$(MAKE) -f $(lastword $(MAKEFILE_LIST)) tesd

main: tesd tesc
	@echo
	@echo
	@echo "Now run 'make install'"
	@echo
	@echo

libs: $(LIBS:%=$(LIB_DEST)/lib%.a) \
	| $(LIB_DEST)

$(BIN_DEST)/tesc: $(BIN_SRC)/tesc.c $(HEADERS) \
	| $(BIN_DEST)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

$(BIN_DEST)/tesd: $(BIN_SRC)/tesd.o $(BIN_SRC)/tesd_tasks.o \
	$(TASKS_OBJ) $(LIBS:%=$(LIB_DEST)/lib%.a) $(HEADERS) \
	| $(BIN_DEST)
	$(CC) $(CFLAGS) $(LDFLAGS) $(filter %.o %.a,$^) \
		$(HDF5LIB) $(LDLIBS) -o $@

$(LIB_DEST)/lib%.a: $(LIB_SRC)/%.o $(HEADERS) \
	| $(LIB_DEST)
	ar rcs $@ $<

$(LIB_DEST) $(BIN_DEST):
	install -d $@

##################################################

tests: $(TEST_PROGS:%=$(BIN_DEST)/%)

# name of program should include any needed libraries
$(BIN_DEST)/%: $(TEST_SRC)/%.o \
	$(LIBS:%=$(LIB_DEST)/lib%.a) $(HEADERS) \
	| $(BIN_DEST)
	$(CC) $(CFLAGS) $(LDFLAGS) $(filter-out %.h,$^) \
		$(foreach lib, \
			$(findstring czmq,$*) \
			$(findstring zmq,$*) \
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
	@echo
	@echo "Now run 'make clean'"
	@echo
	@echo

##################################################

clean:
	rm -f $(LIB_SRC)/*.o $(BIN_SRC)/*.o $(TEST_SRC)/*.o

fullclean:
	rm -f $(LIB_SRC)/*.o $(BIN_SRC)/*.o $(TEST_SRC)/*.o \
		$(BIN_DEST)/* $(LIB_DEST)/*

.DEFAULT_GOAL := main
.PHONY: all main libs tests clean fullclean install
