import "@stdlib/lisp-lists"
import "@stdlib/strings"
import "@stdlib/reflection"

struct Point {
    x: int8 = 10
    y: int8 = 20
}

struct WithRef {
    id: int32?
    ref: Cell<address>?
}

struct WithArrays {
    before: int32?
    a1: array<WithRef>
    a2: array<bool?>?
    after: int32
}

struct WithStrings {
    s: string
    sopt: string?
    sref: Cell<string>
    sarr: array<string>
}

@overflow1023_policy("suppress")
struct LotsOfCoins {
    nums: (coins, coins, coins, coins, coins, coins, coins, coins, coins, coins)
}

struct (0x1234) EmptyE

type ArrayOfLotsCoins = array<LotsOfCoins>

fun `[1..5] each in ref`(): slice asm "<b <b <b <b <b <b x{02C_} s, b> ref, x{824_} s, b> ref, x{81C_} s, b> ref, x{814_} s, b> ref, x{80C_} s, b> ref, x{05C_} s, b> PUSHREF CTOS"
fun `[]_maybeRef`(): slice asm "<b x{004_} s, b> PUSHREF CTOS"
fun `[1..3]_maybeRef`(): slice asm "<b <b x{008101C_} s, b> ref, x{03C_} s, b> PUSHREF CTOS"
fun `[1..10]_maybeRef`(): slice asm "<b <b x{008101820283038404854_} s, b> ref, x{0AC_} s, b> PUSHREF CTOS"

@noinline
fun array<T>.equalTo(self, rhs: array<T>): bool {
    val size = self.size();
    if (size != rhs.size()) {
        return false;
    }
    var i = 0;
    repeat (size) {
        if (self.get(i) != rhs.get(i)) {
            return false;
        }
        i += 1;
    }
    return true;
}

@method_id(101)
fun test1() {
    var a1: array<int8> = [1, 2, 3, 4, 5];
    var a2 = array<int8>.fromCell(a1.toCell());
    return a1.equalTo(a2);
}

@method_id(102)
fun test2(next: int) {
    var arrays: array<array<int8>> = [
        [],
        [1],
        [1,2,3],
        [1,2,3,4,5,next,7,8,9,10],
        [-128,127]
    ];
    var back = arrays.toCell().beginParse().loadAny<array<array<int8>>>();
    assert (arrays.size() == back.size()) throw 123;
    var i = 0;
    repeat (arrays.size()) {
        assert (arrays.get(i).equalTo(back.get(i))) throw 123;
        i += 1;
    }
    return i;
}

@method_id(103)
fun test3() {
    var c1 = array<int32>.toCell([1,2,3,4,5,6,7,8,9,10]);
    var c2 = array<int64>.toCell([1,2,3,4,5,6,7,8,9,10]);
    var c3 = array<int128>.toCell([1,2,3,4,5,6,7,8,9,10]);
    var c4 = array<int200>.toCell([1,2,3,4,5,6,7,8,9,10]);
    var c5 = array<int255>.toCell([1,2,3,4,5,6,7,8,9,10]);
    var c6 = array<int256>.toCell([1,2,3,4,5,6,7,8,9,10]);
    return (c1.depth(), c2.depth(), c3.depth(), c4.depth(), c5.depth(), c6.depth());
}

@method_id(104)
fun test4() {
    var s1 = array<int256>.toCell([]).beginParse();
    var s2 = array<int256>.toCell([1]).beginParse();
    var s3 = array<int256>.toCell([1, 2, 3]).beginParse();
    var s4 = array<int256>.toCell([1, 2, 3, 4]).beginParse();
    return (
        s1.remainingBitsAndRefsCount(),
        s1.skipBits(8).loadMaybeRef(),
        777,
        s2.remainingBitsAndRefsCount(),
        s2.loadRef().beginParse().remainingBitsAndRefsCount(),
        777,
        s3.remainingBitsAndRefsCount(),
        s3.preloadRef().beginParse().remainingBitsAndRefsCount(),
        777,
        s4.preloadRef().beginParse().remainingBitsAndRefsCount(),
        s4.preloadRef().beginParse().loadRef().beginParse().remainingBitsAndRefsCount(),
    )
}

@method_id(105)
fun test5() {
    var points: array<Point> = [
        {},
        {x: 70, y: 80},
    ];
    var s = points.toCell().beginParse();
    return (s.preloadRef().beginParse().remainingBitsCount(), s.loadAny<array<Point>>());
}

@method_id(106)
fun test6() {
    var points: array<Point> = [];
    var i = 0;
    repeat (100) {
        points.push({x: i, y: i});
        i += 1;
    }
    var s = points.toCell().beginParse();
    var coords = array<uint16>.fromSlice(s);
    var pts = array<Point>.fromSlice(s);
    return (
        s.loadUint(8),
        s.loadBool(),
        coords.size(), coords.first(), coords.last(),
        pts.size(), pts.first().x, pts.last().y,
    );
}

@method_id(107)
fun test7() {
    var arr: array<WithRef> = [
        { id: null, ref: address("EQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff+W72r5gqPrHF").toCell() },
        { id: 1, ref: address("EQAUzE-Nef80O9dLZy91HfPiOb6EEQ8YqyWKyIU-KeaYLNUi").toCell() },
        { id: 2, ref: address("EQCIJLNFIko5CvpKn9oAkrDgLocDOoD4vwmHxNx_fsG_LkwW").toCell() },
        { id: null, ref: null },
        { id: 4, ref: address("EQCIJLNFIko5CvpKn9oAkrDgLocDOoD4vwmHxNx_fsG_LkwW").toCell() },
        { id: 5, ref: null },
    ];
    var back = array<WithRef>.fromCell(arr.toCell());
    return (
        back.first().id,
        back.first().ref!.load() == address("EQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff+W72r5gqPrHF"),
        back.last().id,
        back.last().ref,
        back.get(3).id == null,
        back.get(3).ref == null,
        back.get(4).ref == null,
        (back.get(4).ref!.load() as slice).remainingBitsCount(),
    )
}

@method_id(108)
fun test8() {
    var arr: ArrayOfLotsCoins = [
        { nums: (0, 0, 0, 0, 0, 0, 0, 0, 0, 0) },
        { nums: (ton("10"), ton("10"), ton("10"), ton("10"), ton("10"), ton("10"), ton("10"), ton("10"), ton("10"), ton("10")) },
        { nums: (0, 0, 0, 0, 0, 0, 0, 0, 0, ton("0.05")) },
    ];
    arr = ArrayOfLotsCoins.fromCell(arr.toCell());
    return (
        arr.get(0).nums.0,
        arr.get(0).nums.9,
        arr.get(1).nums.0,
        arr.get(1).nums.9,
        arr.get(2).nums.0,
        arr.get(2).nums.9,
    )
}

@method_id(109)
fun test9() {
    var a1: array<RemainingBitsAndRefs> = [
        createEmptySlice(),
        array<int32> [1,2].toCell().beginParse(),
        "hello".literalSlice(),
        array<(coins, coins, coins, coins, cell)> [(0,ton("0.05"),0,0,createEmptyCell()), (1,1,1,1,createEmptyCell()), (2,2,2,2,createEmptyCell())].toCell().beginParse(),
    ];
    var back = array<RemainingBitsAndRefs>.fromCell(a1.toCell());
    var last = array<(coins, coins, coins, coins, cell)>.fromSlice(back.last());
    return (
        back.size(),
        back.get(0).isEmpty(),
        back.get(1).isEmpty(),
        array<int32>.fromSlice(back.get(1)),
        back.get(2).bitsEqual("hello".literalSlice()),
        last.first().1,
        last.last().4.beginParse().isEmpty(),
    )
}

@method_id(110)
fun test10() {
    var o1 = WithArrays.fromCell(WithArrays {
        before: null,
        a1: [
            { id: null, ref: address("EQCIJLNFIko5CvpKn9oAkrDgLocDOoD4vwmHxNx_fsG_LkwW").toCell() }
        ],
        a2: null,
        after: 100,
    }.toCell());
    var o2 = WithArrays.fromCell(WithArrays {
        before: 8,
        a1: [
            { id: 70, ref: null },
            { id: null, ref: null },
        ],
        a2: [true, false],
        after: 2000,
    }.toCell());
    var o3 = WithArrays.fromCell(WithArrays {
        before: null,
        a1: [],
        a2: [],
        after: 9,
    }.toCell());
    return (
        o1.a1.size(),
        o1.a1.get(0).id == null,
        o1.a1.get(0).ref == null,
        o2.a1.last().ref,
        o2.a2,
        o2.after,
        o3,
    )
}

type EmptyShape = []
type MyShape1 = [int32?, Point, EmptyShape, EmptyE, Cell<[]>?, int8]

@method_id(111)
fun test11() {
    var (zuko1: MyShape1, zuko2: MyShape1) = (
        [ 100, {}, [], {}, null, 8 ],
        [ null, {x:-5, y:-15}, [], {}, null, -8 ]
    );
    var b1 = beginCell().storeAny(zuko1);
    var b2 = beginCell().storeBool(true).storeInt(100, 32).storeInt((10<<8)+20, 16).storeUint(0x1234,16).storeBool(false).storeInt(8, 8);
    assert(b1.hash() == b2.hash()) throw 123;
    val o1 = MyShape1.fromCell(zuko1.toCell());
    return (
        o1,
        o1.1,
        o1.2,
        o1.3,
        MyShape1.fromCell(zuko2.toCell()).5,
    )
}

struct Recurrent {
    value: int16
    nextSh: [int4, Cell<Recurrent>?]
}

fun Recurrent.sumAll(self): int {
    var sum = self.value + self.nextSh.0;
    if (self.nextSh.1 != null) {
        sum += self.nextSh.1.load().sumAll();
    }
    return sum;
}

@method_id(112)
fun test12() {
    var r: Recurrent = {
        value: 100,
        nextSh: [1, Recurrent {
            value: 200,
            nextSh: [2, Recurrent {
                value: 300,
                nextSh: [3, null],
            }.toCell()],
        }.toCell()],
    };
    var s = r.toCell().beginParse();
    assert (s.remainingBitsCount() == 21) throw 123;
    assert (s.preloadUint(21) == 0b000000000110010000011) throw 123;
    val r2 = Recurrent.fromSlice(s);
    return (r.sumAll(), r2.sumAll());
}

@method_id(113)
fun test13() {
    var l3: lisp_list<int8> = [];
    l3.prependHead(3);
    l3.prependHead(2);
    l3.prependHead(1);
    var l1: lisp_list<int256> = [];
    l1.prependHead(256);
    return (
        lisp_list<int8>.fromCell(l3.toCell()),
        lisp_list<int256>.fromCell(l1.toCell()),
        lisp_list<address>.fromCell(([] as lisp_list<address>).toCell()),
    )
}

@method_id(114)
fun test14() {
    try {
        var l: lisp_list<int16> = [];
        l.prependHead(0);
        // read int8 instead of int16, assertEnd will fail
        val back = lisp_list<int8>.fromCell(l.toCell());
        return back.getHead() as int;
    } catch (ex) {
        return ex;
    }
}

@method_id(115)
fun test15() {
    // estimation gives "unpredictable infinity" since it's custom logic via packToBuilder
    return reflect.estimateSerializationOf<lisp_list<int8>>()
}

type ArrayList8 = array<lisp_list<int8>>

@method_id(116)
fun test16() {
    val l1: lisp_list<int8> = [1, 2, 3];
    var l2: lisp_list<int8> = [];
    l2.prependHead(4);
    val arr = ArrayList8 [l1, l2];
    val back = arr.toCell().load();
    return (
        back.size(),
        back.first().getTail().getTail(),
        back.last(),
    )
}

@method_id(117)
fun test17() {
    var o1: WithStrings = {
        s: "hello",
        sopt: StringBuilder{}.append("one").append("two").build(),
        sref: "obed".toCell(),
        sarr: ["one", "two"],
    };
    val back1 = o1.toCell().load();
    assert (back1.s.equalTo("hello")) throw 123;
    assert ((back1.sopt as unknown as cell).depth() == 1) throw 123;
    assert (back1.sref.load().beginParse().bitsEqual("obed".literalSlice())) throw 123;
    assert (back1.sarr.last().beginParse().bitsEqual("two".literalSlice())) throw 123;

    val manual = beginCell()
        .storeRef(beginCell().storeSlice("hello".literalSlice()).endCell())
        .storeMaybeRef(beginCell().storeSlice("one".literalSlice()).storeRef(beginCell().storeSlice("two".literalSlice()).endCell()).endCell())
        .storeRef(beginCell().storeRef(beginCell().storeSlice("obed".literalSlice()).endCell()).endCell())
        .storeUint(2, 8)
        .storeMaybeRef(beginCell()
            .storeMaybeRef(null)
            .storeRef(beginCell().storeSlice("one".literalSlice()).endCell())
            .storeRef(beginCell().storeSlice("two".literalSlice()).endCell())
            .endCell())
        .endCell();

    val o2 = WithStrings {
        s: "",
        sopt: null,
        sref: "".toCell(),
        sarr: ["1", "2", "3", "4", "5"]
    }.toCell().load();

    return (
        o1.toCell().hash() == manual.hash(),
        o2.sopt,
        o2.sref.load().calculateLength(),
        o2.sarr.size(),
        o2.sarr.last().hash() & 0xFFFF,
    )
}

@method_id(118)
fun test18() {
    var list = lisp_list<string> ["1", "2"];
    list.prependHead("0");
    return list.toCell().load().calculateSize();
}


fun main() {
    return (
        array<int8>.fromSlice(`[1..5] each in ref`()),
        array<int8>.fromSlice(`[]_maybeRef`()),
        array<int8>.fromSlice(`[1..3]_maybeRef`()),
        array<int8>.fromSlice(`[1..10]_maybeRef`()),
    )
}

/**
@testcase | 0   |     | [ 1 2 3 4 5 ] [] [ 1 2 3 ] [ 1 2 3 4 5 6 7 8 9 10 ]
@testcase | 101 |     | -1
@testcase | 102 | 6   | 5
@testcase | 103 |     | 1 1 2 2 3 4
@testcase | 104 |     | 9 0 (null) 777 9 1 257 0 777 9 1 769 0 777 257 1 769 0
@testcase | 105 |     | 33 [ [ 10 20 ] [ 70 80 ] ]
@testcase | 106 |     | 100 -1 100 0 25443 100 0 99
@testcase | 107 |     | (null) -1 5 (null) -1 -1 0 267 
@testcase | 108 |     | 0 0 10000000000 10000000000 0 50000000 
@testcase | 109 |     | 4 -1 0 [ 1 2 ] -1 50000000 -1
@testcase | 110 |     | 1 -1 0 (null) [ -1 0 ] 2000 (null) [] [] 9
@testcase | 111 |     | [ 100 [ 10 20 ] [] [] (null) 8 ] 10 20 [] -8
@testcase | 112 |     | 606 606
@testcase | 113 |     | [ 1 [ 2 [ 3 (null) ] ] ] [ 256 (null) ] (null)
@testcase | 114 |     | 9
@testcase | 115 |     | 0 9999 0 4
@testcase | 116 |     | 2 [ 3 (null) ] [ 4 (null) ]
@testcase | 117 |     | -1 (null) 0 5 18792
@testcase | 118 |     | 3
 */
