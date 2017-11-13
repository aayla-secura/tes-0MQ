PROJ_ROOT := ../..
include $(PROJ_ROOT)/Makefile

CFLAGS += -fPIC
LDFLAGS += -L.

all: $(so)

install: $(so)
	# this is not one of make's implicit rules, good
	if [ ! -d $(LIB_ROOT) ] ; then mkdir -p $(LIB_ROOT) ; fi
	if [ $? -eq 0 ] ; then cp $(so) $(LIB_ROOT) ; fi

clean:
	# this is not one of make's implicit rules, good
	rm -f *.o $(so)

%.o: %.c
	# this is not one of make's implicit rules, good
	$(CC) $(CFLAGS) -c $< -o $@

lib%.so: %.o
	# this is not one of make's implicit rules, good
	$(CC) $(CFLAGS) $(LDFLAGS) -shared $< -o $@
