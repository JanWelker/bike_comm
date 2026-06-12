# codec2 — drowe67/codec2 v1.2.0 vendored at upstream/.
#
# Pulled subset is the 3200 bps mode dependency tree only. The non-3200
# codebooks (jmv/ge/newamp1/newamp2) and the newamp1.c TU are linked in
# because codec2.c carries inline references to them across all the
# 8 kHz modes; --gc-sections strips most of the unreferenced data when
# we only ever call codec2_create(CODEC2_MODE_3200).
#
# License: BSD-2-Clause, see LICENSE in this directory.

file(GLOB CODEC2_SRCS "upstream/src/*.c")

idf_component_register(
    SRCS
        ${CODEC2_SRCS}
        "codec2_alloc.c"
    INCLUDE_DIRS "upstream/include"
    REQUIRES heap
)

# Disable every codec2 mode except 3200. The non-3200 paths still
# compile, but the CODEC2_MODE_ACTIVE(...) macro collapses to a
# constant 0 in those branches, so the compiler dead-strips the code
# AND the large rodata codebooks the dead code pulled in. Without
# this, codebooknewamp1.c (~200 KB) + codebooknewamp2.c (~200 KB)
# overflow internal DRAM by ~100 KB. We dropped the source files for
# those codebooks; the dispatch macro keeps any residual reference
# constant-folded to 0.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CODEC2_MODE_EN_DEFAULT=0
    CODEC2_MODE_3200_EN=1
    # debug_alloc.h's MALLOC/CALLOC/FREE macros route to extern
    # codec2_malloc/calloc/free when this is set; without it they
    # direct-call libc malloc and stay in internal DRAM. The extern
    # impls live in codec2_alloc.c and route to PSRAM.
    __EMBEDDED__
)

# codec2 upstream pulls in <math.h>, <stdio.h>, debug allocator helpers
# and a pile of unused warnings on Xtensa. Quiet them at the component
# boundary so the rest of the project stays -Wall -Werror-clean.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-maybe-uninitialized
    -Wno-implicit-function-declaration
    -Wno-shift-negative-value
    -Wno-array-bounds
    -Wno-stringop-overflow
    -Wno-stringop-overread
    -Wno-format
    -Wno-strict-aliasing
)
