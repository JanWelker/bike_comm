CC ?= cc
CFLAGS := -O2 -Wall -Wextra -I../../main
SRCS := ../../main/resampler_48_16.c
HDRS := ../../main/resampler_48_16.h

test_resampler: test_resampler.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_resampler.c $(SRCS) -lm

.PHONY: clean
clean:
	rm -f test_resampler
