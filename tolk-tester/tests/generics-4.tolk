fun eqUnusedT<T=null>(v: int) { return v; }
fun eqUnusedU<T, U = never>(v: T) { if (v is int123) { __expect_type(v as U, "never"); } return v; }

fun dup<T1 = int?, T2 = slice?>(x: T1, y: T2): (T1, T2) { return (x, y) }

struct Container1<T=int?> {
    item: T;
}

fun getItemOf<T=never>(v: Container1<T>) {
    return v.item;
}

struct WithDef1<T = null> {
    body: T? = null;
}

fun getBodyOf<TBody = null>(o: WithDef1<TBody>) {
    return o.body;
}

struct WithNever<T = never> {
    f1: int;
    f2: T;
}

fun eqNever<T = never>(o: WithNever<T>) { return o; }

struct MyInit<TInit> {
    value: coins;
    data: TInit;
}

fun MyInit<TInit>.getValue(self) {
    return self.value;
}

struct Parameters<TBody = never, TInit = never> {
    bounce: bool;
    body: TBody;
    init: builder | MyInit<TInit> | null = null;
}

fun createParameters<TBody = null, TInit = null>(bounce: bool, body: TBody, data: TInit): Parameters<TBody, TInit> {
    return { bounce, body, init: { value: ton("0"), data } };
}

fun mySend<TBody = never, TInit = never>(p: Parameters<TBody, TInit>): int {
    var total = 0;
    if (p.bounce) {
        total += 1;
    }
    if (p.body !is never) {
        assert(p.body is TBody, 101);
        total += 10;
    }
    if (p.init is MyInit) {
        total += 100 + p.init.getValue();
    }
    return total;
}

fun test1() {
    eqUnusedT(100);
    eqUnusedU(100);
    eqUnusedU(beginCell());

    __expect_type(dup(null, null), "(null, null)");
    __expect_type(dup(createEmptyTuple(), 6), "(tuple, int)");
}

fun test2() {
    var w1 = Container1 { item: 123 };
    var w2 = Container1 { item: null };
    __expect_type(w1, "Container1<int>");
    __expect_type(w2, "Container1<null>");
    __expect_type(getItemOf(w1), "int");
    __expect_type(getItemOf(w2), "null");

    __expect_type(getItemOf({item: null as slice?}), "slice?");
    __expect_type(getItemOf({item: null}), "null");
}

@method_id(103)
fun test3() {
    __expect_type(WithNever{f1:10}, "WithNever<never>");
    __expect_type(WithNever{f1:10,f2:20}, "WithNever<int>");

    __expect_type(eqNever({f1:10}), "WithNever<never>");
    __expect_type(eqNever({f1:10,f2:20}), "WithNever<int>");
    __expect_type(eqNever({f1:10,f2:null}), "WithNever<null>");

    var a: WithNever<never> = {f1:10};
    return (a, WithNever{f1:20}, eqNever({f1:30}), 777, eqNever({f1:40,f2:40}));
}

@method_id(104)
fun test4() {
    __expect_type(getBodyOf({body: 123}), "int?");
    __expect_type(getBodyOf({body: null}), "null");
    __expect_type(getBodyOf({}), "null");

    __expect_type(WithDef1{}, "WithDef1<null>");
    __expect_type(WithDef1{}.body, "null");
    __expect_type(WithDef1{body: null}.body, "null");
    __expect_type(WithDef1{body: 123}.body, "int?");

    return (getBodyOf({body: null}), getBodyOf({}), WithDef1{});
}

@method_id(105)
fun test5() {
    __expect_type(Parameters { bounce: true }, "Parameters<never, never>");
    __expect_type(Parameters { bounce: false, body: 179 }, "Parameters<int, never>");
    __expect_type(Parameters { bounce: true, init: beginCell() }, "Parameters<never, never>");
    __expect_type(Parameters { bounce: false, body: beginCell(), init: { value: 123, data: 123 } }, "Parameters<builder, int>");

    __expect_type(createParameters(true, null, null), "Parameters<null, null>");
    __expect_type(createParameters(true, beginCell(), "123"), "Parameters<builder, slice>");

    __expect_type(createParameters(true, null, null).body, "null");
    __expect_type(createParameters(true, 123, null).body, "int");
    __expect_type(createParameters(true, null, null).init, "builder | MyInit<null> | null");
    __expect_type(createParameters(true, 123, 456).init, "builder | MyInit<int> | null");

    return (createParameters(true, null, null), 777, createParameters(false, 123, 456));
}

@method_id(106)
fun test6() {
    var p: Parameters<int, cell> = {
        bounce: true,
        body: 123,
        init: { value: ton("0"), data: beginCell().endCell() }
    };
    return (p.body is int, p.init is cell, p.init is MyInit, p.init is MyInit && p.init.data.depth() == 0);
}

@method_id(107)
fun test7() {
    __expect_type(Parameters{ bounce: false, body: 123, init: { value: ton("0"), data: beginCell() }}, "Parameters<int, builder>");
    var v1 = mySend({ bounce: true });
    var v2 = mySend({ bounce: false, body: 123, init: beginCell() });
    var v3 = mySend({ bounce: false, body: 123, init: { value: ton("0"), data: beginCell() }});
    var v4 = mySend({ bounce: true, init: { value: 16, data: null as [int]? }});
    return (v1, v2, v3, v4);
}

fun main() {
}

/**
@testcase | 103 |      | 10 20 30 777 40 40
@testcase | 104 |      | (null) (null) (null)
@testcase | 105 |      | -1 (null) 0 (null) 133 777 0 123 0 456 132
@testcase | 106 |      | -1 0 -1 -1
@testcase | 107 |      | 1 10 110 117

@fif_codegen DECLPROC eqUnusedT<null>
@fif_codegen DECLPROC eqUnusedU<builder,never>
@fif_codegen DECLPROC mySend<never,never>
@fif_codegen DECLPROC mySend<int,never>
@fif_codegen DECLPROC mySend<int,builder>
@fif_codegen DECLPROC mySend<never,[int]?>

@fif_codegen
"""
  test6 PROC:<{                 //
    NEWC                        //  '8
    ENDC                        //  p.init.USlot2
    -1 PUSHINT                  //  p.init.USlot2 '11=-1
    FALSE                       //  p.init.USlot2 '11=-1 '12
    TRUE                        //  p.init.USlot2 '11=-1 '12 '14
    s0 s3 XCHG                  //  '14 '11=-1 '12 p.init.USlot2
    CDEPTH                      //  '14 '11=-1 '12 '19
    0 EQINT                     //  '14 '11=-1 '12 '21
    0 NEQINT                    //  '14 '11=-1 '12 '18
    s1 s3 s0 XCHG3              //  '11=-1 '12 '14 '18
  }>
"""
 */
