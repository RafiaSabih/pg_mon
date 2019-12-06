# contrib/pg_mon/Makefile

MODULE_big = pg_mon
OBJS = pg_mon.o
PGFILEDESC = "pg_mon - monitor queries"
ifdef ENABLE_GCOV
	PG_CPPFLAGS += -g -ggdb -pg -O0 -fprofile-arcs -ftest-coverage
endif
EXTENSION = pg_mon
DATA = pg_mon--1.0.sql


PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
ifdef ENABLE_GCOV
	SHLIB_LINK  += -lgcov --coverage
endif
