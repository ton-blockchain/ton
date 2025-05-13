fun takeAnySlice(slice: slice) {}
fun getSomeSlice() { return beginCell().endCell().beginParse(); }
fun getNullableBytes16(): bytes16? { return getSomeSlice() as bytes16; }

const someBytes = "asdf" as bytes16;
const anotherBytes: bytes16 = "asdf" as bytes16;

fun autoInferBytes16() {
    if (random.uint256()) { return someBytes; }
    else if (random.uint256()) { return true ? "" as bytes16 : anotherBytes; }
    else { return someBytes!; }
}

@method_id(101)
fun test1() {
    var ss = beginCell().storeInt(1, 32).storeInt(2, 32).endCell().beginParse() as bits32;
    __expect_type(ss, "bits32");
    __expect_type(ss as slice, "slice");
    __expect_type(ss as slice?, "slice?");
    __expect_type(ss as bytes128, "bytes128");
    __expect_type(someBytes, "bytes16");
    __expect_type(10>3 ? (null, ss as bits1) : (ss as bytes1, null), "(bytes1?, bits1?)");
    return ((ss as slice).remainingBitsCount(), (ss as slice).loadInt(32), (ss as slice).loadInt(32));
}

fun test2(a: bytes8, b: bits64) {
    a = b as bytes8;
    b = a as bits64;
    (a as slice).loadRef();
    (b as slice?)!.loadRef();
}

@method_id(103)
fun test3() {
    var slice = beginCell().storeInt(1, 32).storeInt(2, 32).endCell().beginParse();
    var bq = slice as bits16?;
    if (bq != null) {
        return (bq as slice).loadInt(32 as uint8) + (slice = bq as slice).remainingBitsCount();
    }
    return -1;
}

fun main() {}

/**
@testcase | 101 |    | 64 1 2
@testcase | 103 |    | 33
 */
