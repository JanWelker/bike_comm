CC ?= cc
CFLAGS := -O2 -Wall -Wextra -I../../main
PROTO_SRCS := ../../main/mesh_proto.c

test_mesh_proto: test_mesh_proto.c $(PROTO_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

.PHONY: clean
clean:
	rm -f test_mesh_proto
