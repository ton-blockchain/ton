struct Ok<T> {
    result: T;
}

struct Err<T> {
    errPayload: T;
}

type OkAlias<T> = Ok<T>;

type Response<TResult, TError> = Ok<TResult> | Err<TError>;

@pure
fun getResponse(success: bool): Response<int, slice> {
    return success ? Ok { result: 10 } : Err { errPayload: beginCell().storeInt(-1,32).endCell().beginParse() };
}

@method_id(101)
fun test1(getOk: bool) {
    return match (var r = getResponse(getOk)) {
        Ok => r.result,
        Err => r.errPayload.loadInt(32)
    };
}

fun test2() {
    var r = getResponse(true);
    match (r) {
        Ok<int> => r as OkAlias<int>,
        Err<slice> => r as Err<slice>,
    }
    match (r) {
        OkAlias<int> => r as Ok<int>,
        Err<slice> => {}
    }
    match (r) {
        Ok => { __expect_type(r, "Ok<int>"); }
        Err => { __expect_type(r, "Err<slice>"); }
    }

    if (r is Ok<int>) { __expect_type(r, "Ok<int>"); }
    if (r is Ok) { __expect_type(r, "Ok<int>"); }
    if (r is OkAlias<int>) { __expect_type(r, "Ok<int>"); }
    if (r !is OkAlias<int>) { __expect_type(r, "Err<slice>"); }
    if (r is Ok<int> && r !is Err<slice> && r is Ok) { __expect_type(r, "Ok<int>"); }
}

@method_id(103)
fun test3() {
    var r: Response<int, slice> = getResponse(true);
    r = Err { errPayload: "" };
    __expect_type(r, "Err<slice>");
    r = OkAlias { result: 10 };
    __expect_type(r, "Ok<int>");
    return (r, r is Ok, (r as Response<int,slice>) is Err);
}

@method_id(104)
fun test4() {
    var r1 = OkAlias { result: OkAlias { result: 10 } } as Response< Ok<int>, slice >;
    match (r1) {
        Ok => { __expect_type(r1, "Ok<Ok<int>>"); r1.result.result; }
        Err => {}
    }
    var r2: Response< Response<int,slice>, slice > = Err { errPayload: "" };
    __expect_type(r2, "Err<slice>");
    r2 = 10>3 ? Ok { result: Ok { result: 12 } } : r2;
    var mm1 = match (r2) {
        OkAlias<Response<int,slice>> => true,
        Err<slice> => false
    };
    var mm2 = match (r2) {
        Ok => true,
        Err => false
    };
    return (r2, mm1, mm2, r2 is Ok<Response<int,slice>>, r2 is OkAlias<OkAlias<int>>, r2 is Err<slice>, r2 is Ok, r2 is Err, r2 !is Ok);
}

@method_id(105)
fun test5() {
    var o1: Ok<int> = { result: 1 };
    var o2: OkAlias<int> = { result: 2 };
    return (o1 is Ok, o1 is OkAlias, o2 is Ok, o2 is OkAlias, o1 is Ok<slice>, o1 is OkAlias<slice>);
}

type OkInt = Ok<int>;

fun test6(w: Response<int, slice> | int) {
    match (w) {
        int => {}
        Ok => {}
        Err => {}
    }
    match (w) {
        OkInt => {}
        int => {}
        Err => {}
    }
}

fun test7(w: OkInt | Err<slice> | int) {
    match (w) {
        Ok => {}
        Err => {}
        int => {}
    }
}

struct WithDef<T> {
    f1: T? = null;
    f2: int? = null as int?;
    f3: T? = null as T?;
    price: coins = ton("0.05"),
    slice: slice = stringHexToSlice("010203"),
}

@method_id(108)
fun test8() {
    var w1: WithDef<slice> = {};
    var w2: WithDef<Ok<int>> = { price: ton("0.1"), f3: { result: 12 } };
    return (w1.price, w1.f1, w1.f3, w2.price, w2.f3!.result, w2.f3, w2.slice.remainingBitsCount());
}


fun main() {
    __expect_type(getResponse(true), "Response<int, slice>");
}

/**
@testcase | 101 | -1      | 10
@testcase | 101 | 0       | -1
@testcase | 103 |         | 10 -1 0
@testcase | 104 |         | 12 129 132 -1 -1 -1 0 0 -1 0 0
@testcase | 105 |         | -1 -1 -1 -1 0 0
@testcase | 108 |         | 50000000 (null) (null) 100000000 12 12 24

@fif_codegen
"""
  test3 PROC:<{
    10 PUSHINT  // r.USlot1=10
    -1 PUSHINT  // r.USlot1=10 '11=-1
    FALSE       // r.USlot1=10 '11=-1 '13
  }>
"""
 */
