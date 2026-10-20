struct CounterReset {
    initialValue: int64;
}

struct CounterIncrement1 {
}

struct CounterIncrement {
    byValue: int32;
}

type MyMessage =
      CounterIncrement
    | CounterIncrement1
    | CounterReset;

fun fakeParseMessage(mode: int): MyMessage {
    __expect_inline(true);
    if (mode == 1) {
        return CounterIncrement1 {};
    } else if (mode > 0) {
        return CounterIncrement { byValue: mode };
    } else {
        return CounterReset { initialValue: 0 };
    }
}

fun onInternalMessage(curCounter: int, mode: int) {
    var m = fakeParseMessage(mode);
    __expect_type(m, "MyMessage");
    
    val nextCounter: int = match (m) {
        CounterReset => m.initialValue,
        CounterIncrement => curCounter + m.byValue,
        CounterIncrement1 => curCounter + 1,
    };
    return nextCounter;
}

/**
@testcase | 0 | 50 0 | 0
@testcase | 0 | 50 1 | 51
@testcase | 0 | 50 9 | 59

@fif_codegen
"""
  onInternalMessage() PROC:<{ 	//  curCounter mode
    DUP
    1 EQINT
    IF:<{
      DROP
      PUSHNULL
      129 PUSHINT
    }>ELSE<{
      DUP
      0 GTINT
      IF:<{
        130 PUSHINT
      }>ELSE<{
        DROP
        0 PUSHINT
        131 PUSHINT
      }>
    }>
"""
 */
