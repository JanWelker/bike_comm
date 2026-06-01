CC ?= cc
LIBLC3 := ../../components/liblc3/upstream
CFLAGS := -O2 -Wall -Wextra -I$(LIBLC3)/include
LIBLC3_SRCS := $(wildcard $(LIBLC3)/src/*.c)

test_codec: test_codec.c $(LIBLC3_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

.PHONY: clean
clean:
	rm -f test_codec
