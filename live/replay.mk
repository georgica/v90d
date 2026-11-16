include Makefile
../live/replay: ../live/replay.c $(CORE_SRC) $(HDRS)
	$(CC) $(CFLAGS) -DP34D_TRACE $(INC) -o $@ ../live/replay.c $(CORE_SRC) $(LDLIBS)
