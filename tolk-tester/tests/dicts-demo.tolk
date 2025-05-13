import "@stdlib/tvm-dicts"

fun cell?.addIntToIDict(mutate self, key: int, number: int): void {
    return self.iDictSetBuilder(32, key, beginCell().storeInt(number, 32));
}

fun calculateDictLen(d: cell?) {
    var len = 0;
    var (k, v, f) = d.uDictGetFirst(32);
    while (f) {
        len += 1;
        (k, v, f) = d.uDictGetNext(32, k!);
    }
    return len;
}

fun slice.loadTwoDigitNumberFromSlice(mutate self): int {
    var n1 = self.loadInt(8);
    var n2 = self.loadInt(8);
    return (n1 - 48) * 10 + (n2 - 48);
}


@method_id(101)
fun test101(getK1: int, getK2: int, getK3: int) {
    var dict = createEmptyDict();
    dict.uDictSetBuilder(32, 1, beginCell().storeUint(1, 32));
    var (old1: slice?, found1) = dict.uDictSetAndGet(32, getK1, beginCell().storeUint(2, 32).endCell().beginParse());
    var (old2: slice?, found2) = dict.uDictSetAndGet(32, getK2, beginCell().storeUint(3, 32).endCell().beginParse());
    var (cur3: slice?, found3) = dict.uDictGet(32, getK3);
    return (
        found1 ? old1!.loadUint(32) : -1,
        found2 ? old2!.loadUint(32) : -1,
        found3 ? cur3!.loadUint(32) : -1
    );
}

@method_id(102)
fun test102() {
    var dict = createEmptyDict();
    dict.addIntToIDict(2, 102);
    dict.addIntToIDict(1, 101);
    dict.addIntToIDict(4, 104);
    dict.addIntToIDict(3, 103);
    var deleted = createEmptyTuple();
    var shouldBreak = false;
    while (!shouldBreak) {
        var (kDel, kVal, wasDel) = dict.iDictDeleteLastAndGet(32);
        if (wasDel) {
            deleted.push([kDel, kVal!.loadInt(32)]);
        } else {
            shouldBreak = true;
        }
    }
    return deleted;
}

@method_id(103)
fun test103() {
    var dict = createEmptyDict();
    dict.uDictSetBuilderIfNotExists(32, 1,beginCell().storeInt(1, 32));
    dict.uDictSetBuilderIfNotExists(32, 1,beginCell().storeInt(1, 32));
    var len1 = calculateDictLen(dict);
    dict.uDictSetBuilderIfExists(32, 2,beginCell().storeInt(1, 32));
    dict.uDictSetBuilderIfExists(32, 2,beginCell().storeInt(1, 32));
    var len2 = calculateDictLen(dict);
    dict.uDictSetBuilder(32, 3,beginCell().storeInt(1, 32));
    dict.uDictSetBuilderIfExists(32, 3,beginCell().storeInt(1, 32));
    var len3 = calculateDictLen(dict);
    var (delK1, _, _) = dict.uDictDeleteFirstAndGet(32);
    var (delK2, _, _) = dict.uDictDeleteFirstAndGet(32);
    var (delK3, _, _) = dict.uDictDeleteFirstAndGet(32);
    return (len1, len2, len3, delK1, delK2, delK3);
}

@method_id(104)
fun test104() {
    var dict = createEmptyDict();
    dict.sDictSetBuilder(32, "7800", beginCell().storeUint(5 + 48, 8).storeUint(6 + 48, 8));
    dict.sDictSet(32, "key1", "12");
    var (old1, _) = dict.sDictSetAndGet(32, "key1", "34");
    var (old2, _) = dict.sDictDeleteAndGet(32, "key1");
    var (restK, restV, _) = dict.sDictGetFirst(32);
    var (restK1, restV1, _) = dict.sDictDeleteLastAndGet(32);
    assert (restK!.bitsEqual(restK1!)) throw 123;
    assert (restV!.bitsEqual(restV1!)) throw 123;
    return (
        old1!.loadTwoDigitNumberFromSlice(),
        old2!.loadTwoDigitNumberFromSlice(),
        restV!.loadTwoDigitNumberFromSlice(),
        restK!.loadTwoDigitNumberFromSlice(),
        restK!.loadTwoDigitNumberFromSlice()
    );
}

fun main() {}

/**
@testcase | 101 | 1 1 1 | 1 2 3
@testcase | 101 | 1 2 1 | 1 -1 2
@testcase | 101 | 1 2 3 | 1 -1 -1
@testcase | 102 |       | [ [ 4 104 ] [ 3 103 ] [ 2 102 ] [ 1 101 ] ] 
@testcase | 103 |       | 1 1 2 1 3 (null)
@testcase | 104 |       | 12 34 56 78 0
 */
