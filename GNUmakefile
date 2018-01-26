PREFIX := /usr/local

LIB_SRC  := $(abspath src/lib)
BIN_SRC  := $(abspath src/bin)
TEST_SRC := $(abspath examples)
CPATH    := $(abspath include)
LIB_DEST := $(abspath lib)
BIN_DEST := $(abspath bin)

PROGS      := tesd tesc
LIBS       := $(patsubst %.c,%,$(notdir $(wildcard $(LIB_SRC)/*.c)))
HEADERS    := $(wildcard $(CPATH)/*.h $(CPATH)/net/*.h)
TEST_PROGS := $(patsubst %.c,%,$(notdir $(wildcard $(TEST_SRC)/*.c)))
TASKS_OBJ  := $(patsubst %.c,%.o,$(wildcard $(BIN_SRC)/tesd_task_*.c))

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

main: $(BIN_DEST)/tesd $(BIN_DEST)/tesc
	@echo
	@echo "Now run 'make install'"

libs: $(LIBS:%=$(LIB_DEST)/lib%.a) \
	| $(LIB_DEST)

$(BIN_DEST)/tesc: $(BIN_SRC)/tesc.c $(HEADERS) \
	| $(BIN_DEST)
	$(CC) $(CFLAGS) $(LDFLAGS) $(filter-out %.h,$^) \
		$(LDLIBS) -o $@

$(BIN_DEST)/tesd: $(BIN_SRC)/tesd.o $(BIN_SRC)/tesd_tasks.o \
	$(TASKS_OBJ) $(LIBS:%=$(LIB_DEST)/lib%.a) $(HEADERS) \
	| $(BIN_DEST)
	$(CC) $(CFLAGS) $(LDFLAGS) $(filter-out %.h,$^) \
		$(HDF5LIB) $(LDLIBS) -o $@

$(LIB_DEST)/lib%.a: $(LIB_SRC)/%.o $(HEADERS) \
	| $(LIB_DEST)
	ar rcs $@ $(filter-out %.h,$^)

$(LIB_DEST) $(BIN_DEST):
	install -d $@

##################################################

test: $(TEST_PROGS:%=$(BIN_DEST)/%)

# name of program should include any needed libraries
$(BIN_DEST)/%: $(TEST_SRC)/%.o \
	$(LIBS:%=$(LIB_DEST)/lib%.a) $(HEADERS) \
	| $(BIN_DEST)
	$(CC) $(CFLAGS) $(LDFLAGS) $(filter-out %.h,$^) \
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
