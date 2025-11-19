@noinline
fun eqUnusedT<T=null>(v: int) { return v; }
@noinline
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

struct WithVoid<T = void> {
    f1: int;
    f2: T;
}

fun eqVoid<T = void>(o: WithVoid<T>) { return o; }

struct MyInit<TInit> {
    value: coins;
    data: TInit;
}

fun MyInit<TInit>.getValue(self) {
    return self.value;
}

struct Parameters<TBody = void, TInit = void> {
    bounce: bool;
    body: TBody;
    init: builder | MyInit<TInit> | null = null;
}

fun createParameters<TBody = null, TInit = null>(bounce: bool, body: TBody, data: TInit): Parameters<TBody, TInit> {
    return { bounce, body, init: { value: ton("0"), data } };
}

fun void.assertIsVoid(self) {
    __expect_type(self, "void");
}

fun Parameters<int>.methodIntVoid(self) {
    __expect_type(self, "Parameters<int, void>");
}

fun Parameters<int>.genericIntVoid<T1, T2 = slice>(self): (T2?, T1?) {
    return (null, null)
}

fun multipleWithDefaults<T1, T2, T3 = int, T4 = builder>(): [T1, T2, T3, T4]? {
    return null
}

struct MultipleWithDefaults<T1, T2, T3 = int?, T4 = builder> {
    t: [T1, T2, T3, T4]? = null
}

fun MultipleWithDefaults<T1, T2>.getT(self) {
    return self.t
}

fun MultipleWithDefaults<T1, T2, T3, T4>.method1<U1 = int>(self) {
    return [self.t!.2, null as U1?]
}

fun MultipleWithDefaults<T1, T2, T3, T4>.method2<U1, U2 = int>(self) {
    return [self.t!.3, null as U1?, null as U2?]
}

type ParametrizedInt<T1, T2 = slice> = int

@noinline
fun mySend<TBody = void, TInit = void>(p: Parameters<TBody, TInit>): int {
    var total = 0;
    if (p.bounce) {
        total += 1;
    }
    if (p.body !is void) {
        assert(p.body is TBody, 101);
        total += 10;
    }
    if (p.init is MyInit) {
        total += 100 + p.init.getValue();
    }
    return total;
}

@method_id(101)
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
    __expect_type(WithVoid{f1:10}, "WithVoid<void>");
    __expect_type(WithVoid{f1:10,f2:20}, "WithVoid<int>");

    __expect_type(eqVoid({f1:10}), "WithVoid<void>");
    __expect_type(eqVoid({f1:10,f2:20}), "WithVoid<int>");
    __expect_type(eqVoid({f1:10,f2:null}), "WithVoid<null>");

    var a: WithVoid<void> = {f1:10};
    return (a, WithVoid{f1:20}, eqVoid({f1:30}), 777, eqVoid({f1:40,f2:40}));
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
    __expect_type(Parameters { bounce: true }, "Parameters<void, void>");
    __expect_type(Parameters { bounce: false, body: 179 }, "Parameters<int, void>");
    __expect_type(Parameters { bounce: true, init: beginCell() }, "Parameters<void, void>");
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
    __expect_type(Parameters{ bounce: false }, "Parameters<void, void>");
    __expect_type(Parameters{ bounce: false, body: 123, init: { value: ton("0"), data: beginCell() }}, "Parameters<int, builder>");
    __expect_type(Parameters{ bounce: false, body: 123 }.genericIntVoid<builder>(), "(slice?, builder?)");
    __expect_type(Parameters{ bounce: false, body: 123 }.genericIntVoid<builder, cell>(), "(cell?, builder?)");
    __expect_type(Parameters{ bounce: false }.body, "void");
    Parameters{ bounce: false }.body.assertIsVoid();

    var v1 = mySend({ bounce: true });
    var v2 = mySend({ bounce: false, body: 123, init: beginCell() });
    var v3 = mySend({ bounce: false, body: 123, init: { value: ton("0"), data: beginCell() }});
    var v4 = mySend({ bounce: true, init: { value: 16, data: null as [int]? }});
    return (v1, v2, v3, v4);
}

struct FakeGeneric8<T> {
    alwaysInt: int;
}

struct Snake8 {
    next: FakeGeneric8<Snake8>;     // it's not a recursive struct, it's okay
    next2: FakeGeneric8<Snake8>?;
    next3: FakeGeneric8<FakeGeneric8<Snake8>>;
}

@method_id(108)
fun test8() {
    var sn: Snake8 = {
        next: { alwaysInt: 10 },
        next2: null,
        next3: { alwaysInt: 20 }
    };
    return sn;
}

fun main() {
    var cb2 = multipleWithDefaults<int, cell>;
    var cb3 = multipleWithDefaults<int, cell, slice>;
    var cb4 = multipleWithDefaults<int, cell, slice, tuple>;
    __expect_type(cb2, "() -> [int, cell, int, builder]?");
    __expect_type(cb3, "() -> [int, cell, slice, builder]?");
    __expect_type(cb4, "() -> [int, cell, slice, tuple]?");
    
    var o2 = MultipleWithDefaults<int, cell>{}; 
    var o3 = MultipleWithDefaults<int, cell, slice>{}; 
    var o4 = MultipleWithDefaults<int, cell, slice?, tuple>{};
    __expect_type(o2.t, "[int, cell, int?, builder]?");
    __expect_type(o3.t, "[int, cell, slice, builder]?");
    __expect_type(o4.t, "[int, cell, slice?, tuple]?");
    
    __expect_type(o2.getT(), "[int, cell, int?, builder]?");
    
    __expect_type(o4.method1(), "[slice?, int?]");
    __expect_type(o4.method1<Container1<never>>(), "[slice?, Container1<never>?]");
    __expect_type(o4.method2<cell>(), "[tuple, cell?, int?]");
    __expect_type(o4.method2<cell, Container1<void>>(), "[tuple, cell?, Container1<void>?]");

    __expect_type(o2.method2<builder>, "(MultipleWithDefaults<int, cell, int?, builder>) -> [builder, builder?, int?]");

    __expect_type(12 as ParametrizedInt<builder>, "ParametrizedInt<builder, slice>");
    __expect_type(12 as ParametrizedInt<builder, cell>, "ParametrizedInt<builder, cell>");
}

/**
@testcase | 103 |      | 10 20 30 777 40 40
@testcase | 104 |      | (null) (null) (null)
@testcase | 105 |      | -1 (null) 0 (null) typeid-2 777 0 123 0 456 typeid-1
@testcase | 106 |      | -1 0 -1 -1
@testcase | 107 |      | 1 10 110 117
@testcase | 108 |      | 10 (null) 20

@fif_codegen DECLPROC eqUnusedT<null>()
@fif_codegen DECLPROC eqUnusedU<builder,never>()
@fif_codegen DECLPROC mySend<void,void>()
@fif_codegen DECLPROC mySend<int,void>()
@fif_codegen DECLPROC mySend<int,builder>()
@fif_codegen DECLPROC mySend<void,[int]?>()

@fif_codegen
"""
  test6() PROC:<{               //
    <b b> PUSHREF               //  p.init.USlot2
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
