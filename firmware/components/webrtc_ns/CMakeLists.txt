# ESP-IDF component wrapper around vendored cpuimage/WebRTC_NS.
# See upstream/ for the source files, VENDORING.md for the import
# notes.

idf_component_register(
    SRCS "upstream/noise_suppression.c"
    INCLUDE_DIRS "upstream"
)
