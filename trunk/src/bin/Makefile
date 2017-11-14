PROJ_ROOT := ../..
include $(PROJ_ROOT)/Makefile.inc

LDLIBS = $(ext_libs:%=-l% )

all: $(bins)

install: $(bins:%=install-%)

install-%:
	install -d $(BIN_ROOT)
	install $* $(BIN_ROOT)/$*

clean:
	rm -f *.o $(bins)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%: %.c

server_main: server_main.o
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) $(libs:%=-l% ) -o $@

client: client.o
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@
