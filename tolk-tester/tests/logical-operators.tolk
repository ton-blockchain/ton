fun simpleAllConst() {
    return (!0, !!0 & !false, !!!0, !1, !!1, !-1, !!-1, (!5 == 0) == !0, !0 == true);
}

fun compileTimeEval1(x: int) {
    // todo now compiler doesn't understand that bool can't be equal to number other than 0/-1
    // (but understands that it can't be positive)
    // that's why for now, the last condition is evaluated at runtime
    return (!x, !x > 10, !x < 10, !!x == 5, !x == -10);
}

@method_id(101)
fun withIfNot(x: int, y: int) {
    if (!x) { return 10; }
    else if (!y) { return 20; }
    return x+y;
}

@method_id(102)
fun withAndOr(x: int, y: int, z: int) {
    var return_at_end = -1;
    if (!x & !y) {
        if (!z & !y) { return 10; }
        else if (z | !!y) { return_at_end = 20; }
    } else if (!!x & !!y & !z) {
        if (!z & (x > 10)) { return_at_end = 30; }
        if ((x != 11) & !z) { return 40; }
        return_at_end = 50;
    } else {
        return_at_end = !x ? !y : !z | 1;
    }
    return return_at_end;
}

@method_id(103)
fun someSum(upto: int) {
    var x = 0;
    var should_break = false;
    while (!x & !should_break) {
        if (upto < 10) { x = upto; should_break = true; }
        else { upto = upto - 1; }
    }
    return x;
}


fun lookupIdxByValue(idict32: cell, value: int) {
    var cur_key = -1;
    do {
        var (cur_key redef, cs: slice, found: int) = idict32.idict_get_next?(32, cur_key);
        // todo one-line condition (via &) doesn't work, since right side is calculated immediately
        if (found) {
            if (cs~load_int(32) == value) {
                return cur_key;
            }
        }
    } while (found);
    return -1;
}

@method_id(104)
fun testDict(last: int) {
    // prepare dict: [3 => 30, 4 => 40, 5 => 50]
    var dict: cell = new_dict();
    dict~idict_set_builder(32, 3, begin_cell().store_int(30, 32));
    dict~idict_set_builder(32, 4, begin_cell().store_int(40, 32));
    dict~idict_set_builder(32, 5, begin_cell().store_int(!last ? 100 : last, 32));

    return (lookupIdxByValue(dict, 30), lookupIdxByValue(dict, last), lookupIdxByValue(dict, 100));
}

@method_id(105)
fun testNotNull(x: int) {
    return [x == null, null == x, !(x == null), null == null, +(null != null)];
}

fun main() {

}

/**
@testcase | 101 | 0 0    | 10
@testcase | 101 | 5 0    | 20
@testcase | 101 | 5 8    | 13
@testcase | 102 | 0 0 0  | 10
@testcase | 102 | 0 0 5  | 20
@testcase | 102 | 1 2 0  | 40
@testcase | 102 | 11 2 0 | 50
@testcase | 102 | 1 0 0  | -1
@testcase | 102 | 0 1 0  | 0
@testcase | 102 | 1 0 1  | 1
@testcase | 103 | 15     | 9
@testcase | 103 | 6      | 6
@testcase | 103 | -1     | -1
@testcase | 104 | 50     | 3 5 -1
@testcase | 104 | 100    | 3 5 5
@testcase | 104 | 0      | 3 -1 5
@testcase | 105 | 0      | [ 0 0 -1 -1 0 ]
@testcase | 105 | null   | [ -1 -1 0 -1 0 ]

@fif_codegen
"""
  simpleAllConst PROC:<{
    //
    -1 PUSHINT
    0 PUSHINT
    -1 PUSHINT
    0 PUSHINT
    -1 PUSHINT
    0 PUSHINT
    -1 PUSHINT
    TRUE
    TRUE
  }>
"""

@fif_codegen
"""
  compileTimeEval1 PROC:<{
    //  x
    DUP	//  x x
    0 EQINT	//  x _1
    FALSE	//  x _1 _4
    TRUE	//  x _1 _4 _7
    FALSE	//  x _1 _4 _7 _11
    s0 s4 XCHG	//  _11 _1 _4 _7 x
    0 EQINT	//  _11 _1 _4 _7 _12
    -10 EQINT	//  _11 _1 _4 _7 _14
    s3 s4 XCHG
    s1 s3 s0 XCHG3	//  _1 _4 _7 _11 _14
  }>
"""

@fif_codegen
"""
  withIfNot PROC:<{
    c2 SAVE
    SAMEALTSAVE	//  x y
    OVER	//  x y x
    IFNOTJMP:<{	//  x y
      2DROP	//
      10 PUSHINT	//  _2=10
    }>	//  x y
    DUP	//  x y y
    IFNOTJMP:<{	//  x y
      2DROP	//
      20 PUSHINT	//  _3=20
      RETALT
    }>	//  x y
    ADD	//  _4
  }>
"""

 */
