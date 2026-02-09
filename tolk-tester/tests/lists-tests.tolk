import "@stdlib/lisp-lists"

struct Point {
    x: int = 10
    y: int = 20
}

struct WithCb<R> {
    cb: (int, int) -> R
}

struct WithTwoCells {
    value: int32
    r1: Cell<int32>
    r2: Cell<any_address>
}

fun eq<T>(v: T) { return v }

/// For testing purposes only (not included in stdlib). Assumes list depth < 255.
/// Converts from `[1 [ 2 [ 3 null ] ] ]` to `[1 2 3]`
fun lisp_list<T>.toArray(self): array<T> {
    var result = array<T> [];
    var it = self;
    while (!it.isEmpty()) {
        result.push(it.popHead());
    }
    return result;
}

fun lisp_list<T>.serializeAsSeqInt8(self): int {
    var b = beginCell();
    var it = self;
    while (!it.isEmpty()) {
        b.storeAny<int8>(it.popHead());
    }
    val s = b.toSlice();
    return s.preloadUint(s.remainingBitsCount());
}

fun lisp_list<int>.allLessThan(self, cmp: int): bool {
    var it = self;
    while (!it.isEmpty()) {
        if (it.popHead() >= cmp) {
            return false;
        }
    }
    return true;
}

fun lisp_list<address>.containsAddress(self, a: address): bool {
    var it = self;
    while (!it.isEmpty()) {
        if (a == it.popHead()) {
            return true;
        }
    }
    return false;
}

fun lisp_list<T>.makeEmpty(mutate self) {
    self = [];
}

fun lisp_list<T1>.convertFrom<T2>(mutate self, rhs: lisp_list<T2>): self {
    var reversed = lisp_list<T1> [];
    var it = rhs;
    while (!it.isEmpty()) {
        reversed.prependHead(it.popHead() as T1);
    }
    self = reversed.calculateReversed();
    return self;
}

@method_id(101)
fun test101() {
    var n = lisp_list<int> [3, 2, 1];
    return (
        n,
        n.getHead(),
        n.getTail(),
        n.popHead(),
        n.calculateSize(),
        n.isEmpty(),
        n.calculateReversed(),
    )
}

@method_id(102)
fun test102() {
    var numbers1 = [1, 2, 3, 4] as lisp_list<int>;
    assert (numbers1.serializeAsSeqInt8() == 0x01020304) throw 123;

    var numbers2 = createEmptyList<int>();
    numbers2.prependHead(4);
    numbers2.prependHead(3);
    numbers2.prependHead(2);
    numbers2.prependHead(1);
    return numbers1.serializeAsSeqInt8() == numbers2.serializeAsSeqInt8();
}

@method_id(103)
fun test103() {
    var numbers1 = lisp_list<int32> [1, 2, 3, 4];
    return (
        numbers1.toArray(),
        numbers1.calculateReversed().toArray(),
    );
}

@method_id(104)
fun test104() {
    var l = lisp_list<int> [];
    l.prependHead(10);
    l.prependHead(20);
    return (
        l.allLessThan(40),
        l.allLessThan(20),
        l.popHead(),
        l.allLessThan(20),
        l.makeEmpty(),
        l.calculateSize(),
        l.serializeAsSeqInt8(),
    )
}

@method_id(105)
fun test105() {
    val second = address("EQBiMNL9qNWMAkJHuM0BFneYcuHL17kzS4pswpaEO-NGWrFG");
    var l: lisp_list<address> = createEmptyList();
    l.prependHead(address("0:ca6e321c7cce9ecedf0a8ca2492ec8592494aa5fb5ce0387dff96ef6af982a3e"));
    l.prependHead(second);
    return (
        l.isEmpty(),
        l.calculateSize(),
        l.containsAddress(second),
        l.getTail().containsAddress(second),
        l.getHead().getWorkchain(),
        l.containsAddress(second),
        l.popHead().getWorkchain(),
        l.containsAddress(second),
        l.makeEmpty(),
        l,
        l.isEmpty(),
        l.toArray(),
    )
}

@method_id(106)
fun test106() {
    try {
        return (lisp_list<bool> []).getTail().getHead() as int
    } catch (ex) {
        return ex
    }
}

@method_id(107)
fun test107() {
    var l1 = lisp_list<int> [1, 2];
    var l2 = lisp_list<int> [1, 2, 3];
    var backup = l1;
    l1 = l2;
    l2 = backup;
    return (l1.calculateSize(), l2.calculateSize(), l1.toArray().size(), l1.toArray(), l2.toArray());
}

@method_id(108)
fun test108() {
    var l1 = [1, 2] as lisp_list<int8>;
    var l2 = [true, false] as lisp_list<bool>;
    var l3 = lisp_list<address> [address("EQBiMNL9qNWMAkJHuM0BFneYcuHL17kzS4pswpaEO-NGWrFG")];
    var l4 = [1, 2] as lisp_list<int>;
    var l5 = [1, 2] as lisp_list<int>;
    var l6 = [1, 2] as lisp_list<int>;
    var l7 = [1, 2] as lisp_list<int>;
    (l1, l2, l3) = ([], [], []);
    l4 = l5 = [];
    [l6, l7] = [[], []];
    return (l1, l2, l3, l4, l5, l6, l7);
}

@method_id(109)
fun test109() {
    var n = [10] as lisp_list<int>;
    var nn = [[]] as [[]];
    var eq = eq([n] = [[]]);
    return (n, eq);
}

@method_id(110)
fun test110() {
    var l1 = [1, 2, 3] as lisp_list<int>;
    return (
        l1.calculateReversed().toArray(),
        l1.calculateReversed().calculateReversed().toArray(),
    )
}

@method_id(111)
fun test111() {
    val l1 = [1, 2, 3] as lisp_list<int>;
    val l2 = [4, 5, 6] as lisp_list<int>;
    val concat = l1.calculateConcatenation(l2);
    return (
        l1.calculateSize(),
        concat.calculateSize(),
        concat.serializeAsSeqInt8() == 0x010203040506,
        l2.calculateConcatenation(l2).calculateConcatenation(l1.calculateReversed()).toArray(),
        l1.getTail().calculateConcatenation([]).toArray(),
        (lisp_list<int> []).calculateConcatenation(l2.getTail()),
        (lisp_list<address> []).calculateConcatenation([]),
        (lisp_list<address> []).calculateConcatenation([]).isEmpty(),
    )
}

@method_id(112)
fun test112() {
    var outer: lisp_list<lisp_list<int>> = [
        [1, 2, 3],
        [4, 5, 6],
    ];
    outer.prependHead([100]);
    outer.popHead();
    outer.prependHead([]);
    
    var sum = 0;
    var outerIt = outer;
    while (!outerIt.isEmpty()) {
        var innerIt = outerIt.popHead();
        while (!innerIt.isEmpty()) {
            sum += innerIt.popHead();
        }
    }
    return (outer.calculateSize(), sum)
}

@method_id(113)
fun test113(keepNull: bool) {
    var l = keepNull ? null : lisp_list<int> [];
    __expect_type(l, "lisp_list<int>?");
    val copy = l;
    if (l != null) {
        l.prependHead(123);
    }
    return (copy, 777, l);
}

@method_id(114)
fun test114() {
    var a1 = [1, 2, 3] as lisp_list<int>;
    var a2 = [4, [null, [null, null] as unknown]] as tuple as unknown as lisp_list<int?>;
    var a3: lisp_list<int?> = a1 as unknown as lisp_list<int?>;
    var merged = a3.calculateConcatenation(a2);
    var sum = 0;
    var it = merged;
    while (!it.isEmpty()) {
        val elem = it.popHead();
        sum += elem == null ? 100 : elem;
    }
    return sum;
}

@method_id(115)
fun test115() {
    var anything = lisp_list<unknown> [beginCell().storeUint(123, 32), 123];
    return (anything.popHead() as builder).toSlice().loadUint(32)
         + (anything.popHead() as int) 
         + (anything.isEmpty() as int)
         + (anything.calculateSize())
}

@method_id(116)
fun test116() {
    var l1: lisp_list<[int, bool]> = [];
    l1.prependHead([10, true]);
    l1.prependHead([20, false]);
    __expect_type(l1.toArray, "(lisp_list<[int, bool]>) -> array<[int, bool]>");
    var l2: lisp_list<(int, bool)> = [(2, true), (3, false)];
    return (l1.getHead(), l1.calculateReversed().toArray(), l2);
}

@method_id(117)
fun test117() {
    var points = lisp_list<Point> [
        {x: 100, y: 200},
        {},
    ];
    return (
        points.popHead(),
        points,
        points.getTail(),
        points.popHead().y,
        points.isEmpty(),
    )
}

@method_id(118)
fun test118() {
    var lis = lisp_list<Point?> [{}, null];
    lis = lis.calculateReversed();
    lis.prependHead({y:70});
    val arr = lis.toArray();
    __expect_type(arr, "array<Point?>");
    return (arr, arr.last()!, arr.get(1) == null);
}

@method_id(119)
fun test119() {
    var l1: lisp_list<int?>;
    var l2: lisp_list<Point?>;
    return (eq(((l1, l2) = ([], [])).0), l1, l2, l1.isEmpty());
}

@method_id(120)
fun test120() {
    var ints = [2, 3] as lisp_list<int>;
    var nul_ints = [] as lisp_list<int?>;
    nul_ints.convertFrom(ints).prependHead(null);
    
    var nulls = lisp_list<null> [null];
    var nul_points = lisp_list<Point?> [];
    nul_points.convertFrom(nulls).prependHead({});
    
    return (
        nul_ints.toArray(), 
        nul_points.toArray(), 
        nul_points.getHead(), 
        nul_points.popHead() is Point, 
        nul_points.popHead() is Point,
        nul_points.calculateSize(),
    );
}

@method_id(121)
fun test121() {
    var lis: int | lisp_list<int -> WithCb<int>> = [
        fun(a) {
            __expect_type(a, "int");
            return { cb: fun(a, b) {
                __expect_type(b, "int");
                return a + b
            } };
        },
        fun(a) { return { cb: fun(a, b) { return a * b }}},
    ];
    __expect_type(lis, "lisp_list<(int) -> WithCb<int>>");
    var result = 0;
    while (!lis.isEmpty()) {
        result += lis.popHead()(0).cb(5, 6);
    }
    return result;
}

@method_id(122)
fun test122() {
    val c = beginCell().storeUint(171, 8).endCell();
    val lis = [c] as lisp_list<cell>;
    val back = lisp_list<cell>.fromCell(lis.toCell());
    return back.getHead().hash() == c.hash();
}

@method_id(123)
fun test123() {
    var lis: lisp_list<WithTwoCells> = [
        {
            value: 100,
            r1: (100 as int32).toCell(),
            r2: createAddressNone().toCell(),
        },
        {
            value: 200,
            r1: (200 as int32).toCell(),
            r2: createAddressNone().toCell(),
        },
    ];
    lis = lis.toCell().load();
    val (first, second) = (lis.popHead(), lis.popHead());
    return (
        lis.isEmpty(),
        first.value,
        second.value,
        first.r1.load(),
        second.r2.load().isNone(),
    )
}

const list1: lisp_list<int> = []
const list2: lisp_list<int> = [1, 2]
const list1Unknown: unknown = list1
const list2Unknown: unknown = list2

@method_id(124)
fun test124() {
    __expect_type(list2, "lisp_list<int>");
    return (
        list1,
        list2,
        list1Unknown,
        list2Unknown,
        (list1 as unknown) == null,
    )
}



fun main() {
    __expect_type([] as lisp_list<int8>, "lisp_list<int8>");
    __expect_type([[{}]] as lisp_list<lisp_list<Point>>, "lisp_list<lisp_list<Point>>");
    __expect_type(lisp_list<int8> [], "lisp_list<int8>");
    __expect_type(lisp_list<lisp_list<Point>> [[{}]], "lisp_list<lisp_list<Point>>");
}

/**
@testcase | 101 |       | [ 3 [ 2 [ 1 (null) ] ] ] 3 [ 2 [ 1 (null) ] ] 3 2 0 [ 1 [ 2 (null) ] ]
@testcase | 102 |       | -1
@testcase | 103 |       | [ 1 2 3 4 ] [ 4 3 2 1 ]
@testcase | 104 |       | -1 0 20 -1 0 0
@testcase | 105 |       | 0 2 -1 0 0 -1 0 0 (null) -1 []
@testcase | 106 |       | 7
@testcase | 107 |       | 3 2 3 [ 1 2 3 ] [ 1 2 ]
@testcase | 108 |       | (null) (null) (null) (null) (null) (null) (null)
@testcase | 109 |       | (null) [ (null) ]
@testcase | 110 |       | [ 3 2 1 ] [ 1 2 3 ]
@testcase | 111 |       | 3 6 -1 [ 4 5 6 4 5 6 3 2 1 ] [ 2 3 ] [ 5 [ 6 (null) ] ] (null) -1
@testcase | 112 |       | 3 21
@testcase | 113 | -1    | (null) 0 777 (null) 0
@testcase | 113 | 0     | (null) 129 777 [ 123 (null) ] 129
@testcase | 114 |       | 210
@testcase | 115 |       | 245
@testcase | 116 |       | [ 20 0 ] [ [ 10 -1 ] [ 20 0 ] ] [ [ 2 -1 ] [ [ 3 0 ] (null) ] ]
@testcase | 117 |       | 100 200 [ [ 10 20 ] (null) ] (null) 20 -1
@testcase | 118 |       | [ [ 10 70 130 ] [ (null) (null) 0 ] [ 10 20 130 ] ] 10 20 -1
@testcase | 119 |       | (null) (null) (null) -1
@testcase | 120 |       | [ (null) 2 3 ] [ [ 10 20 130 ] [ (null) (null) 0 ] ] 10 20 130 -1 0 0
@testcase | 121 |       | 41
@testcase | 122 |       | -1
@testcase | 123 |       | -1 100 200 100 -1
@testcase | 124 |       | (null) [ 1 [ 2 (null) ] ] (null) [ 1 [ 2 (null) ] ] -1
 */
