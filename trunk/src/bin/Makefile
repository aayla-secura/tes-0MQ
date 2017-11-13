PROJ_ROOT := ../..
include $(PROJ_ROOT)/Makefile

LDLIBS = $(ext_libs:%=-l% )

all: $(bins)

install: $(bins)
	# this is not one of make's implicit rules, good
	if [ ! -d $(BIN_ROOT) ] ; then mkdir -p $(BIN_ROOT) ; fi
	if [ $? -eq 0 ] ; then cp $(bins) $(BIN_ROOT) ; fi

clean:
	# this is not one of make's implicit rules, good
	rm -f *.o $(bins)

%.o: %.c
	# this is not one of make's implicit rules, good
	$(CC) $(CFLAGS) -c $< -o $@

%: %.c

server_main: server_main.o
	# this is not one of make's implicit rules, good
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) $(libs:%=-l% ) -o $@

client: client.o
	# this is not one of make's implicit rules, good
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@
