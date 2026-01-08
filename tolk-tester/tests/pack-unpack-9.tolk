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
        "",
        array<int32> [1,2].toCell().beginParse(),
        "hello",
        array<(coins, coins, coins, coins, cell)> [(0,ton("0.05"),0,0,createEmptyCell()), (1,1,1,1,createEmptyCell()), (2,2,2,2,createEmptyCell())].toCell().beginParse(),
    ];
    var back = array<RemainingBitsAndRefs>.fromCell(a1.toCell());
    var last = array<(coins, coins, coins, coins, cell)>.fromSlice(back.last());
    return (
        back.size(),
        back.get(0).isEmpty(),
        back.get(1).isEmpty(),
        array<int32>.fromSlice(back.get(1)),
        back.get(2).bitsEqual("hello"),
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
 */
