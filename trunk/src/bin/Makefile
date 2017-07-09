PROJ_ROOT := ../..
include $(PROJ_ROOT)/Makefile

LDLIBS += $(libs:%=-l% )

all: $(bins)

install: $(bins)
	# this is not one of make's implicit rules, good
	if [[ ! -d $(BIN_ROOT) ]] ; then mkdir $(BIN_ROOT) ; fi
	mv $(bins) $(BIN_ROOT)

clean:
	# this is not one of make's implicit rules, good
	rm -rf *.o $(bins)

%.o: %.c
	# this is not one of make's implicit rules, good
	$(CC) $(CFLAGS) -c $< -o $@

%: %.c

%: %.o
	# this is not one of make's implicit rules, good
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@
