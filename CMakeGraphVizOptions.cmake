# https://cmake.org/cmake/help/latest/module/CMakeGraphVizOptions.html

set(GRAPHVIZ_EXTERNAL_LIBS FALSE)

# exclude the targets that result in too many connections
# example: liba;libb_;libc_sublib
set(GRAPHVIZ_IGNORE_TARGETS absl_;)

