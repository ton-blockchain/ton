fun int.incChained(mutate self): self {
    self = self + 1;
    return self;
}

fun int.incChained2(mutate self): self {
    return self.incChained();
}

fun int.incChained3(mutate self): self {
    self.incChained();
    return self;
}

fun int.incChained4(mutate self): self {
    self.incChained();
    return self;
}

@method_id(101)
fun testIncChainedCodegen(x: int) {
    return x.incChained().incChained2().incChained3().incChained4();
}

@method_id(102)
fun testIncChained() {
    var x: int = 10;
    x.incChained();
    x.incChained();
    x.incChained2();
    x.incChained2().incChained();
    x = x.incChained();
    x = x.incChained2().incChained().incChained2();
    return x.incChained();
}

fun int.incChainedWithMiddleReturn(mutate self, maxValue: int): self {
    if (self >= maxValue) {
        return self;
    }
    self += 1;
    return self;
}

@method_id(103)
fun testIncChainedWithMiddleReturn(x: int) {
    x.incChainedWithMiddleReturn(10).incChainedWithMiddleReturn(10);
    x = x.incChainedWithMiddleReturn(10).incChainedWithMiddleReturn(10);
    return x.incChainedWithMiddleReturn(10).incChainedWithMiddleReturn(999);
}

fun int.incChainedMutatingBoth(mutate self, mutate y: int): self {
    self += 1;
    y += 1;
    return self;
}

global c104: int;

@method_id(104)
fun testIncChainedMutatingBoth() {
    var (x, y) = (0, 0);
    c104 = 0;
    x.incChainedMutatingBoth(mutate y).incChainedMutatingBoth(mutate y);
    x.incChainedMutatingBoth(mutate y);
    x = x.incChainedMutatingBoth(mutate c104).incChainedMutatingBoth(mutate c104).incChainedMutatingBoth(mutate y);
    return (x, y, c104);
}

fun (int, int).incTensorChained(mutate self): self {
    val (f, s) = self;
    self = (f + 1, s + 1);
    return self;
}

@method_id(105)
fun testIncTensorChained(f: int, s: int) {
    var tens = (f, s);
    tens.incTensorChained().incTensorChained();
    return tens.incTensorChained().incTensorChained();
}

fun int.incConditionalChainable(mutate self, mutate another: int, ifLessThan: int): self {
    another += 1;
    return self.incChained() < ifLessThan ? self.incChained().incChained() : self;
}

@method_id(106)
fun testIncConditionalChainable(x: int) {
    var y = 0;
    x.incConditionalChainable(mutate y, 5).incConditionalChainable(mutate y, 5);
    x = x.incConditionalChainable(mutate y, 5).incConditionalChainable(mutate y, 5);
    return (x.incConditionalChainable(mutate y, 5), y);
}

fun int.checkNotEq(self, throwIfEq: int): void {
    if (self == throwIfEq) {
        throw 100 + throwIfEq;
    }
}

@method_id(107)
fun testNotMutatingSelf(arg: int) {
    try {
        arg.checkNotEq(100);
        arg.checkNotEq(101);
        arg.checkNotEq(102);
        return 0;
    } catch (code) {
        return code;
    }
}

global c108: int;

fun int.checkNotEqChainable(self, throwIfEq: int): self {
    c108 += 1;
    if (self != throwIfEq) {
        return self;
    }
    throw 100 + throwIfEq;
    return self;
}

@method_id(108)
fun testNotMutatingChainableSelf(arg: int) {
    c108 = 0;
    try {
        arg.checkNotEqChainable(100).checkNotEqChainable(101).checkNotEqChainable(102);
        arg = arg.checkNotEqChainable(100).checkNotEqChainable(101).checkNotEqChainable(102);
        return (arg, c108);
    } catch (code) {
        return (code, c108);
    }
}

global onceFailed109: int;

fun int.checkNotEqChainableMutateAnother(self, throwIfEq: int, mutate toInc: int): self {
    if (onceFailed109) { return self; }
    toInc += 1;
    try { return self.checkNotEqChainable(throwIfEq); }
    catch { onceFailed109 = 1; return self; }
}

global c109: int;

@method_id(109)
fun testNotMutatingChainableSelfMutateAnother(initial: int) {
    val arg = initial;
    var x = 0;
    c108 = 0;
    c109 = 0;
    onceFailed109 = 0;
    arg.checkNotEqChainableMutateAnother(100, mutate x)
       .checkNotEqChainableMutateAnother(101, mutate c109)
       .checkNotEqChainableMutateAnother(102, mutate x);
    return (arg, c108, c109, x);
}

fun int.pickG110(mutate self, mutate pushTo: tuple): self {
    self += 10;
    pushTo.push(c110);
    return self;
}

global tup110: tuple;
global c110: int;

@method_id(110)
fun testMutateGlobalsLValue(init: int) {
    c110 = init;
    tup110 = createEmptyTuple();
    c110.incChained().incChained().pickG110(mutate tup110).incChained().pickG110(mutate tup110).incChained();
    return (c110, tup110);
}

fun tuple.myTuplePush<T>(mutate self, value: T): self {
    self.push(value);
    return self;
}

fun tuple.myTupleAt<T>(self, idx: int): T {
    return self.get(idx);
}

global tup111: tuple;

@method_id(111)
fun testForallFunctionsWithSelf(): (int, int, tuple) {
    var t = createEmptyTuple();
    tup111 = createEmptyTuple();
    t.myTuplePush(10);
    tup111.myTuplePush(1).myTuplePush(2).myTuplePush(3);
    return (t.myTupleAt(0), tup111.myTupleAt(tup111.size() - 1), tup111);
}



fun main() { }

/**
@testcase | 101 | 5   | 9
@testcase | 102 |     | 20
@testcase | 103 | 1   | 7
@testcase | 103 | 100 | 101
@testcase | 103 | 8   | 11
@testcase | 104 |     | 6 4 2
@testcase | 105 | 1 2 | 5 6
@testcase | 106 | -20 | -5 5
@testcase | 106 | -1  | 8 5
@testcase | 106 | 7   | 12 5
@testcase | 107 | 200 | 0
@testcase | 107 | 102 | 202
@testcase | 108 | 200 | 200 6
@testcase | 108 | 101 | 201 0
@testcase | 109 | 200 | 200 3 1 2
@testcase | 109 | 100 | 100 0 0 1
@testcase | 109 | 102 | 102 2 1 2
@testcase | 110 | 0   | 24 [ 2 13 ]
@testcase | 111 |     | 10 3 [ 1 2 3 ]

@fif_codegen
"""
  int.incChained PROC:<{    //  self
    INC	                    //  self
  }>
  int.incChained2 PROC:<{   //  self
    int.incChained CALLDICT	//  self
  }>
  int.incChained3 PROC:<{   //  self
    int.incChained CALLDICT	//  self
  }>
  int.incChained4 PROC:<{   //  self
    int.incChained CALLDICT	//  self
  }>
"""

@fif_codegen
"""
  testIncChainedCodegen PROC:<{ // x
    int.incChained CALLDICT	    //  x
    int.incChained2 CALLDICT	//  x
    int.incChained3 CALLDICT	//  x
    int.incChained4 CALLDICT	//  x
  }>
"""
 */
