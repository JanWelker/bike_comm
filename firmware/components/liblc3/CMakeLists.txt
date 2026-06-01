# ESP-IDF component wrapper around vendored google/liblc3.
# See upstream/ for the unmodified source tree, VENDORING.md for the
# subtree command used to import it.

file(GLOB LIBLC3_SRCS "upstream/src/*.c")

idf_component_register(
    SRCS ${LIBLC3_SRCS}
    INCLUDE_DIRS "upstream/include"
)
