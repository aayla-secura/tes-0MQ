PROJ_ROOT := ../..
include $(PROJ_ROOT)/Makefile.inc

CFLAGS += -fPIC
LDFLAGS += -L.

all: $(so)

install: $(so)
	# this is not one of make's implicit rules, good
	if [ ! -d $(LIB_ROOT) ] ; then mkdir -p $(LIB_ROOT) ; fi
	cp $(so) $(LIB_ROOT)

clean:
	# this is not one of make's implicit rules, good
	rm -f *.o $(so)

%.o: %.c
	# this is not one of make's implicit rules, good
	$(CC) $(CFLAGS) -c $< -o $@

lib%.so: %.o
	# this is not one of make's implicit rules, good
	$(CC) $(CFLAGS) $(LDFLAGS) -shared $< -o $@
