fun main(): int {
    var c: cell = my_begin_cell().storeInt(demo_10, 32).my_end_cell();
    var cs: slice = my_begin_parse(c);
    var ten: int = cs.loadInt(32);
    return 1 + demo1(ten) + demo_var;
}

@pure
fun my_begin_cell(): builder
asm "NEWC";
@pure
fun builder.my_end_cell(self): cell
asm "ENDC";
@pure
fun my_begin_parse(c: cell): slice
asm "CTOS";

fun demo1(v: int): int {
    demo_var = 23;
    return v;
}

global demo_var: int;
const demo_10: int = 10;

fun test1(): int {
    var demo_var: int = demo_10;
    var demo_slice: int = demo_20;
    if (demo_var > 0) {
        var demo_var: tuple? = null;
        var demo_slice: tuple? = null;
    }
    return demo_var + demo_slice;
}

fun test2() {
    return second;
}

global demo_slice: slice;
const demo_20: int = 20;

const second = first + 1;
const first = 1;

/**
@testcase | 0 |  | 34

@fif_codegen
"""
  test1 PROC:<{
    30 PUSHINT  // '10
  }>
"""

@fif_codegen
"""
  test2 PROC:<{
    2 PUSHINT  // '2
  }>
"""
 */
