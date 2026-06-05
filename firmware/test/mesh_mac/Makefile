CC ?= cc
CFLAGS := -O2 -Wall -Wextra -I../../main
PROTO_SRCS := ../../main/mesh_proto.c
PROTO_HDRS := ../../main/mesh_proto.h

test_mesh_proto: test_mesh_proto.c $(PROTO_SRCS) $(PROTO_HDRS)
	$(CC) $(CFLAGS) -o $@ test_mesh_proto.c $(PROTO_SRCS)

.PHONY: clean
clean:
	rm -f test_mesh_proto
