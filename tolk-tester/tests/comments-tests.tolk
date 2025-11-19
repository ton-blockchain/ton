
struct WithDef {
    f1: int,
    f2: int = 10,
    f3: int,
    f4: int,
}

const C_20 = 20;

fun demo_fields_def(x: int) {
    var w: WithDef = {
        f1: x + C_20,
        f3: 100,
        f4: C_20
    };
    return (w.f4 + 5, w.f3, w.f2, w.f1);
}

@noinline
fun add(x: int, y: int = 0) {
    return x + y
}

fun demo_call_def(x: int) {
    var x2 = x * x;
    return add(x2);
}

struct MyOptions {
    calcAbs: bool = true
    coeff: int
}

@noinline
fun some_calc(
    x: int,
    options: MyOptions = { coeff: 2 },
) {
    if (options.calcAbs) {
        x = abs(x);
    }
    return x * options.coeff;
}

@method_id(101)
fun call_some_calc() {
    return some_calc(-10);
}

fun main() {
    var cb = demo_fields_def;
    demo_call_def;
    return 1;
}

/**
@testcase | 0   |   | 1
@testcase | 101 |   | 20

@fif_codegen_enable_comments
@fif_codegen
"""
  demo_fields_def() PROC:<{   	//  x
    // 13: f1: x + C_20
    20 ADDCONST             	//  '6
    // 4: f2: int = 10
    10 PUSHINT              	//  '6 '7=10
    // 14: f3: 100
    100 PUSHINT              	//  w.f1 w.f2=10 w.f3=100
    // 17: return (w.f4 + 5, w.f3, w.f2, w.f1)
    25 PUSHINT                  //  w.f1 w.f2=10 w.f3=100 '11
    s2 s3 XCHG2                 //  '11 w.f3=100 w.f2=10 w.f1
  }>
"""

@fif_codegen
"""
  demo_call_def() PROC:<{       //  x
    // 26: var x2 = x * x
    DUP                         //  x x
    MUL                         //  x2
    // 21: fun add(x: int, y: int = 0)
    0 PUSHINT                   //  x2 '3=0
    // 27: return add(x2)
    add() CALLDICT              //  '4
  }>
"""

@fif_codegen
"""
  call_some_calc() PROC:<{      // 
    // 48: return some_calc(-10)
    -10 PUSHINT                 //  '0=-10
    // 31: calcAbs: bool = true
    TRUE                        //  '0=-10 '1
    // 38: options: MyOptions = { coeff: 2 }
    2 PUSHINT                   //  '0=-10 '1 '2=2
    // 48: return some_calc(-10)
    some_calc() CALLDICT        //  '3
  }>
"""

*/