SHARED_OBJS := $(patsubst %.c,lib%.so,$(wildcard $(LIB_SRC)/*.c))

all-libs: $(SHARED_OBJS)

install-libs: $(SHARED_OBJS:$(LIB_SRC)/=install-lib-)

install-lib-%: | $(LIB_DEST)
	install $(LIB_SRC)/$* $(LIB_DEST)/$*

$(LIB_DEST):
	install -d $(LIB_DEST)

clean-libs:
	rm -f $(SHARED_OBJS:lib%.so=%.o) $(SHARED_OBJS)

fullclean-libs: clean-libs
	rm -f $(SHARED_OBJS:$(LIB_SRC)=$(LIB_DEST))

lib%.so: %.o
	$(CC) $(CFLAGS) $(LDFLAGS) -shared $< -o $@

.PHONY:: all-libs clean-libs fullclean-libs install-libs $(SHARED_OBJS:$(LIB_SRC)/=install-lib-)
