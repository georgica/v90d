# live/replay: the server core with the modem's trace hooks in. The modem is
# libv90modem built with -DP34D_TRACE (and the bench experiments live/*.sh
# put in CFLAGS); the modem's side of the report is libv90modem/tools/trace.c.
LIBV90M_DEFS := -DP34D_TRACE $(filter-out -D_POSIX_C_SOURCE%,$(filter -D%,$(CFLAGS)))
include Makefile
../live/replay: ../live/replay.c $(CORE_SRC) $(HDRS) $(LIBV90M_DEV) $(LIBV90M)/tools/trace.c $(LIBV90M)/tools/trace.h
	$(CC) $(CFLAGS) -DP34D_TRACE $(INC) -I$(LIBV90M)/tools -o $@ ../live/replay.c $(LIBV90M)/tools/trace.c $(CORE_SRC) $(LIBV90M_DEV) $(LDLIBS)
