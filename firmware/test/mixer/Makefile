CC ?= cc
CFLAGS := -O2 -Wall -Wextra -I../../main
JB_SRCS := ../../main/mixer_jb.c

test_mixer: test_mixer.c $(JB_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

.PHONY: clean
clean:
	rm -f test_mixer
