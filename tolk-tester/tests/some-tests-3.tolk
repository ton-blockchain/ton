@method_id(101)
fun test1(cs: slice) {
    return cs.loadUint(8)+cs.loadUint(8)+cs.loadUint(8)+cs.loadUint(8);
}

@method_id(102)
fun test2(cs: slice) {
  var (x0, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10,
       x11, x12, x13, x14, x15, x16, x17, x18, x19) = f(cs);
  return x0 + x1 + x2 + x3 + x4 + x5 + x6 + x7 + x8 + x9
       + x10+ x11+ x12+ x13+ x14+ x15+ x16+ x17+ x18+ x19;
}

fun main(cs: slice) {
    return (cs.loadUint(8), cs.loadUint(8), cs.loadUint(8), cs.loadUint(8));
}

@noinline
fun f(cs: slice) {
    return (cs.loadUint(8), cs.loadUint(8), cs.loadUint(8), cs.loadUint(8),
        cs.loadUint(8), cs.loadUint(8), cs.loadUint(8), cs.loadUint(8),
        cs.loadUint(8), cs.loadUint(8), cs.loadUint(8), cs.loadUint(8),
        cs.loadUint(8), cs.loadUint(8), cs.loadUint(8), cs.loadUint(8),
        cs.loadUint(8), cs.loadUint(8), cs.loadUint(8), cs.loadUint(8));
}


/**
    method_id   | in | out
@testcase | 102 | x{000102030405060708090a0b0c0d0e0f10111213} | 190
@testcase | 101 | x{000102030405060708090a0b0c0d0e0f10111213} | 6
@testcase | 0   | x{000102030405060708090a0b0c0d0e0f10111213} | 0 1 2 3

@code_hash 110494322917670257289501183090765859207486616122530510808569169535563021169176
*/
