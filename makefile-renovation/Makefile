PROJ_ROOT := .
include $(PROJ_ROOT)/common.mk
include $(LIB_SRC)/targets.mk
include $(BIN_SRC)/targets.mk
include $(TESTBIN_SRC)/targets.mk

all clean fullclean install: %: %-libs %-progs
