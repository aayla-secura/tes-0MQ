PROJ_ROOT := ../..
include $(PROJ_ROOT)/Makefile.inc

CFLAGS += -fPIC
LDFLAGS += -L.

all: $(so)

install: $(so:%=install-%)

install-%:
	install -d $(LIB_ROOT)
	install $* $(LIB_ROOT)/$*

clean:
	rm -f *.o $(so)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

lib%.so: %.o
	$(CC) $(CFLAGS) $(LDFLAGS) -shared $< -o $@
