CC ?= cc
CFLAGS := -O2 -Wall -Wextra -I../../main
JB_SRCS := ../../main/mixer_jb.c
JB_HDRS := ../../main/mixer_jb.h

test_mixer: test_mixer.c $(JB_SRCS) $(JB_HDRS)
	$(CC) $(CFLAGS) -o $@ test_mixer.c $(JB_SRCS)

.PHONY: clean
clean:
	rm -f test_mixer
