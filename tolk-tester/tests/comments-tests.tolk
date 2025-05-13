
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

fun main() {
    return 1;
}

/**
@testcase | 0   |   | 1

@fif_codegen_enable_comments
@fif_codegen
"""
  demo_fields_def PROC:<{   	//  x
    // 13: f1: x + C_20
    20 ADDCONST             	//  '6
    10 PUSHINT              	//  '6 '7=10
    // 14: f3: 100
    100 PUSHINT              	//  w.f1 w.f2=10 w.f3=100
    // 17: return (w.f4 + 5, w.f3, w.f2, w.f1)
    25 PUSHINT                  //  w.f1 w.f2=10 w.f3=100 '11
    s2 s3 XCHG2                 //  '11 w.f3=100 w.f2=10 w.f1
  }>
"""

*/