import "imports/some-math"
import "./imports/use-dicts.ext"

fun main() {
    prepareDict_3_30_4_40_5_x(4);
    return someAdd(1, 2);
}

/**
@testcase | 0 | | 3

@fif_codegen
"""
  main() PROC:<{
    3 PUSHINT
  }>
"""
 */
