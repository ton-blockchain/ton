
struct MaybeNothing {}
struct MaybeJust<T> { value: T; }
type Maybe<T> = MaybeNothing | MaybeJust<T>;

struct EitherLeft<T> { value: T }
struct EitherRight<T> { value: T }
type Either<L, R> = EitherLeft<L> | EitherRight<R>;

@inline
fun makeExternalAddress(hash: int, len: int): address {
    return beginCell().storeUint(0b01, 2).storeUint(len, 9).storeUint(hash, len).endCell().beginParse() as address;
}


fun slice.assertEqDeeply(self, rhs: slice): slice {
    var lhs = self;
    assert(lhs.bitsEqual(rhs), 400);
    assert(lhs.remainingRefsCount() == rhs.remainingRefsCount(), 400);
    while (lhs.remainingRefsCount()) {
        lhs.loadRef().beginParse().assertEqDeeply(rhs.loadRef().beginParse());
    }
    return self;
}

@inline
fun generateSlice_44_with_ref45(): slice {
    return generateCell_44_with_ref45().beginParse();
}

@inline
fun generateCell_44_with_ref45(): cell {
    return beginCell().storeInt(44, 32).storeRef(beginCell().storeInt(45, 32).endCell()).endCell();
}

fun assert_slice_is_44_and_ref45(s: slice) {
    assert(s.loadInt(32) == 44, 400);
    var ref = s.loadRef().beginParse();
    assert(ref.loadInt(32) == 45, 400);
    ref.assertEnd();
    s.assertEnd();
}

@inline
fun slice.appendRef(self, refSlice: slice): slice {
    return beginCell().storeSlice(self).storeRef(beginCell().storeSlice(refSlice).endCell()).endCell().beginParse();
}

fun run<TInputStruct>(input: TInputStruct, ans: slice) {
    repeat (2) {
        var s = input.toCell().beginParse();
        input = TInputStruct.fromSlice(s.assertEqDeeply(ans));
    }
    input.toCell().beginParse().skipAny<TInputStruct>().assertEnd();
}


/*
    value:int32
    = JustInt32;
*/

struct JustInt32 {
    value: int32;
}

/*
    value:(Maybe int32)
    = JustMaybeInt32;
 */

struct JustMaybeInt32 {
    value: int32?;
}

/*
    op:int32
    amount:Grams
    = TwoInts32AndCoins;
*/

struct TwoInts32AndCoins {
    op: int32;
    amount: coins;
}

/*
    op:int32
    query_id:uint64
    = TwoInts32And64;
*/

struct TwoInts32And64 {
    op: int32;
    query_id: uint64;
}

/*
    op:int32
    query_id_ref: ^[uint64]
    = TwoInts32AndRef64;
*/

struct TwoInts32AndRef64 {
    op: int32;
    query_id_ref: Cell<uint64>;
}

/*
    op:int32
    query_id:(Maybe uint64)
    demo_bool_field:Bool
    = TwoInts32AndMaybe64;
*/

struct TwoInts32AndMaybe64 {
    op: int32;
    query_id: uint64?;
    demo_bool_field: bool;
}

/*
    addr:MsgAddressInt
    = JustAddress;
*/

struct JustAddress {
    addr: address;
}

/*
    op:int32
    addr:MsgAddressExt
    query_id:uint64
    = TwoInts32And64SepByAddress;
*/

struct TwoInts32And64SepByAddress {
    op: int32;
    addr_e: address;
    query_id: uint64;
}

/*
    op:int32
    i8or256:(Either int8 int256)
    = IntAndEitherInt8Or256;
*/

struct IntAndEitherInt8Or256 {
    op: int32;
    i8or256: int8 | int256;
}

/*
    query_id_ref:uint64 = Inner1;
    i64_in_ref:int64 = Inner2;

    op:int32
    i32orRef:(Either int32 ^Inner2)
    query_id_maybe_ref: (Maybe ^Inner1)
    = IntAndEither32OrRef64;
*/

struct Inner1 {
    query_id_ref: uint64;
}
struct Inner2 {
    i64_in_ref: int64;
}

struct IntAndEither32OrRef64 {
    op: int32;
    i32orRef: int32 | Cell<Inner2>;
    query_id_maybe_ref: Cell<Inner1>?;
}

/*
    value:(Either int8 (Maybe int256))
    op:int32
    = IntAndEither8OrMaybe256;
*/

struct IntAndEither8OrMaybe256 {
    value: Either<int8, int256?>;
    op: int32;
}

/*
    value:(Either (Maybe int8) int256)
    op:int32
    = IntAndEitherMaybe8Or256;
*/

struct IntAndEitherMaybe8Or256 {
    value: Either<int8?, int256>;
    op: int32;
}

/*
    value:(Maybe (Maybe int8))
    op:int32
    = IntAndMaybeMaybe8;
*/

struct IntAndMaybeMaybe8 {
    value: Maybe<Maybe<int8>>;
    op: int32;
}

/*
    f1:bits8
    f2:bits3
    f3:(Maybe bits20)
    f4:(Either bits100 bits200)
    = SomeBytesFields;
*/

struct SomeBytesFields {
    f1: bytes1;
    f2: bits3;
    f3: bits20?;
    f4: bits100 | bits200;
}

/*
    op:int32
    rest:Cell
    = IntAndRestInlineCell;
*/

struct IntAndRestInlineCell {
    op: int32;
    rest: RemainingBitsAndRefs;
}

/*
    op:int32
    rest:^Cell
    = IntAndRestRefCell;
*/

struct IntAndRestRefCell {
    op: int32;
    rest: cell;
}

/*
    op:int32
    rest:(Either Cell ^Cell)
    = IntAndRestEitherCellOrRefCell;
*/

struct IntAndRestEitherCellOrRefCell {
    op: int32;
    rest: RemainingBitsAndRefs | cell;
}

/*
    op:int32
    ref1m:(Maybe ^Cell)
    ref2m:(Maybe ^Cell)
    ref3:^Cell
    ref4m32:(Maybe ^JustInt32)
    query_id:int64
    = DifferentMaybeRefs;
 */

struct DifferentMaybeRefs {
    op: int32;
    ref1m: cell?;
    ref2m: dict;
    ref3: cell;
    ref4m32: Cell<JustInt32>?;
    query_id: int64;
}

/*
    ji:JustInt32
    jmi:JustMaybeInt32
    jiMaybe:(Maybe JustInt32)
    jmiMaybe:(Maybe JustMaybeInt32)
    = DifferentIntsWithMaybe;
*/

struct DifferentIntsWithMaybe {
    ji: JustInt32;
    jmi: JustMaybeInt32;
    jiMaybe: JustInt32?;
    jmiMaybe: JustMaybeInt32?;
}

/*
    ja1:JustAddress
    ja2m:(Maybe JustAddress)
    imm:IntAndMaybeMaybe8
    tis:TwoInts32And64SepByAddress
    = DifferentMix1;
*/

@overflow1023_policy("suppress")
struct DifferentMix1 {
    ja1: JustAddress;
    ja2m: JustAddress?;
    ext_nn: address;
    imm: IntAndMaybeMaybe8;
    tis: TwoInts32And64SepByAddress;
}

/*
    iae:^IntAndEither32OrRef64
    tic:TwoInts32AndCoins
    rest:Cell
    = DifferentMix2;
*/

struct DifferentMix2 {
    iae: Cell<IntAndEither32OrRef64>;
    tic: TwoInts32AndCoins;
    rest: RemainingBitsAndRefs;
}

/*
    bod:(Either ^TwoInts32AndCoins ^JustInt32)
    tim:(Maybe TwoInts32AndCoins)
    pairm:(Maybe (Both int32 int64))
    = DifferentMix3;
 */

struct DifferentMix3 {
    bod: Cell<TwoInts32AndCoins> | Cell<JustInt32>;
    tim: TwoInts32AndCoins?;
    pairm: (int32, int64)?;
}

/*
    Test: write with builder, read with other struct
    (type `builder` is available for writing, but not for reading)
 */

struct WriteWithBuilder {
    f1: int32;
    rest: builder;
}

struct ReadWrittenWithBuilder {
    f1: int32;
    someInt: uint32;
    someCell: cell?;
}

struct ReadWriteRest<T> {
    f1: int32;
    f2: coins;
    rest: T;
}

struct Tail224 {
    ji: JustInt32,
    addr: address,
    ref1: Cell<int32>?,
    ref2: Cell<JustAddress>,
}

type ReadRest_Remaining = ReadWriteRest<RemainingBitsAndRefs>;

struct ReadWriteMid<T> {
    f1: int32;
    mid: T;
    f3: coins;
}



// ---------------------------------------------


@method_id(200)
fun test_JustInt32_1() {
    var b = beginCell();
    b.storeAny(JustInt32{ value: 123 });
    b.storeAny(JustInt32{ value: 456 });
    return b;
}

@method_id(201)
fun test_JustInt32() {
    run<JustInt32>({ value: 255}, stringHexToSlice("000000FF"));
    var s = stringHexToSlice("0000007b000001c8");
    return (s.loadAny<JustInt32>(), s.loadAny<JustInt32>());
}

@method_id(202)
fun test_JustMaybeInt32() {
    run<JustMaybeInt32>({ value: 255 }, stringHexToSlice("8000007FC_"));
    run<JustMaybeInt32>({ value: null }, stringHexToSlice("4_"));
    return true;
}

@method_id(203)
fun test_TwoInts32AndCoins() {
    run<TwoInts32AndCoins>({ op: 123, amount: 0 }, stringHexToSlice("0000007B0"));
    run<TwoInts32AndCoins>({ op: 123, amount: 1000000000 }, stringHexToSlice("0000007B43B9ACA00"));
    return true;
}

@method_id(204)
fun test_TwoInts32And64() {
    run<TwoInts32And64>({ op: 123, query_id: 255 }, stringHexToSlice("0000007B00000000000000FF"));
    return true;
}

@method_id(205)
fun test_TwoInts32AndRef64() {
    run<TwoInts32AndRef64>({ op: 123, query_id_ref: { tvmCell: beginCell().storeUint(255,64).endCell() } }, stringHexToSlice("0000007B").appendRef(stringHexToSlice("00000000000000FF")));
    return true;
}

@method_id(206)
fun test_TwoInts32AndMaybe64() {
    run<TwoInts32AndMaybe64>({ op: 123, query_id: 255, demo_bool_field: true }, stringHexToSlice("0000007B800000000000007FE_"));
    run<TwoInts32AndMaybe64>({ op: 123, query_id: null, demo_bool_field: true }, stringHexToSlice("0000007B6_"));
    run<TwoInts32AndMaybe64>({ op: 123, query_id: null, demo_bool_field: false }, stringHexToSlice("0000007B2_"));
    return true;
}

@method_id(207)
fun test_JustAddress() {
    run<JustAddress>({ addr: address("0:ca6e321c7cce9ecedf0a8ca2492ec8592494aa5fb5ce0387dff96ef6af982a3e") }, stringHexToSlice("80194DC6438F99D3D9DBE151944925D90B2492954BF6B9C070FBFF2DDED5F30547D_"));
    return true;
}

@method_id(208)
fun test_TwoInts32And64SepByAddress() {
    run<TwoInts32And64SepByAddress>({ op: 123, addr_e: makeExternalAddress(70, 10), query_id: 255 } , stringHexToSlice("0000007B41423000000000000007FC_"));
    run<TwoInts32And64SepByAddress>({ op: 123, addr_e: makeExternalAddress(666, 20), query_id: 254 } , stringHexToSlice("0000007B4280053400000000000001FD_"));
    run<TwoInts32And64SepByAddress>({ op: 123, addr_e: createAddressNone(), query_id: 253 } , stringHexToSlice("0000007B000000000000003F6_"));
    return true;
}

@method_id(209)
fun test_IntAndEitherInt8Or256() {
    run<IntAndEitherInt8Or256>({ op: 123, i8or256: 80 as int8 }, stringHexToSlice("0000007B284_"));
    run<IntAndEitherInt8Or256>({ op: 123, i8or256: 65535 as int256 }, stringHexToSlice("0000007B8000000000000000000000000000000000000000000000000000000000007FFFC_"));
    return true;
}

@method_id(210)
fun test_IntAndEither32OrRef64() {
    run<IntAndEither32OrRef64>({ op: 123, i32orRef: Inner2{ i64_in_ref: 555 }.toCell(), query_id_maybe_ref: Inner1{ query_id_ref: 888 }.toCell() }, stringHexToSlice("0000007BE_").appendRef(stringHexToSlice("000000000000022B")).appendRef(stringHexToSlice("0000000000000378")));
    run<IntAndEither32OrRef64>({ op: 123, i32orRef: Inner2{ i64_in_ref: 555 }.toCell(), query_id_maybe_ref: null }, stringHexToSlice("0000007BA_").appendRef(stringHexToSlice("000000000000022B")));
    run<IntAndEither32OrRef64>({ op: 123, i32orRef: 555, query_id_maybe_ref: Inner1{ query_id_ref: 888 }.toCell() }, stringHexToSlice("0000007B00000115E_").appendRef(stringHexToSlice("0000000000000378")));
    run<IntAndEither32OrRef64>({ op: 123, i32orRef: 555, query_id_maybe_ref: null }, stringHexToSlice("0000007B00000115A_"));
    return IntAndEither32OrRef64.fromSlice(stringHexToSlice("0000007B00000115A_"));
}

@method_id(211)
fun test_IntAndEither8OrMaybe256() {
    run<IntAndEither8OrMaybe256>({ value: EitherLeft { value: 100 }, op: 123 }, stringHexToSlice("320000003DC_"));
    run<IntAndEither8OrMaybe256>({ value: EitherRight { value: 10000 }, op: 123 }, stringHexToSlice("C0000000000000000000000000000000000000000000000000000000000009C40000001EE_"));
    run<IntAndEither8OrMaybe256>({ value: EitherRight { value: null }, op: 123 }, stringHexToSlice("8000001EE_"));
    return IntAndEither8OrMaybe256.fromSlice(stringHexToSlice("8000001EE_"));
}

@method_id(212)
fun test_IntAndEitherMaybe8Or256() {
    run<IntAndEitherMaybe8Or256>({ value: EitherLeft { value: 100 }, op: 123 }, stringHexToSlice("590000001EE_"));
    run<IntAndEitherMaybe8Or256>({ value: EitherRight { value: 10000 }, op: 123 }, stringHexToSlice("80000000000000000000000000000000000000000000000000000000000013880000003DC_"));
    run<IntAndEitherMaybe8Or256>({ value: EitherLeft { value: null }, op: 123 }, stringHexToSlice("0000001EE_"));
    return IntAndEitherMaybe8Or256.fromSlice(stringHexToSlice("0000001EE_"));
}

@method_id(213)
fun test_SomeBytesFields() {
    run<SomeBytesFields>({ f1: stringHexToSlice("A4") as bytes1, f2: stringHexToSlice("7_") as bits3, f3: null, f4: stringHexToSlice("BBA87684B3DAA58C0FCC75230C4302C9D156102139D631FF56") as bits200 }, stringHexToSlice("A46DDD43B4259ED52C607E63A9186218164E8AB08109CEB18FFAB4_"));
    run<SomeBytesFields>({ f1: stringHexToSlice("E6") as bytes1, f2: stringHexToSlice("D_") as bits3, f3: stringHexToSlice("2531C") as bits20, f4: stringHexToSlice("927E88FAB2D327D9468547217") as bits100 }, stringHexToSlice("E6D2531C493F447D596993ECA342A390BC_"));
    return SomeBytesFields.fromSlice(stringHexToSlice("E6D2531C493F447D596993ECA342A390BC_")).f4 is bits100;
}

@method_id(214)
fun test_IntAndMaybeMaybe8() {
    run<IntAndMaybeMaybe8>({ value: MaybeJust { value: MaybeJust { value: 88 } }, op: 123 }, stringHexToSlice("D60000001EE_"));
    run<IntAndMaybeMaybe8>({ value: MaybeJust { value: MaybeNothing{} }, op: 123 }, stringHexToSlice("8000001EE_"));

    val t1 = IntAndMaybeMaybe8.fromSlice(stringHexToSlice("D60000001EE_"));  // (88) 123
    val v1 = t1.value;
    __expect_type(v1, "Maybe<Maybe<int8>>");
    assert(v1 is MaybeJust && v1.value is MaybeJust && v1.value.value == 88 && t1.op == 123, 400);
    __expect_type(v1, "MaybeJust<Maybe<int8>>");
    __expect_type(v1.value, "MaybeJust<int8>");
    __expect_type(v1.value.value, "int8");
    val t2 = IntAndMaybeMaybe8.fromSlice(stringHexToSlice("8000001EE_"));  // (()) 123
    val v2 = t2.value;
    assert(v2 is MaybeJust && v2.value is MaybeNothing && t2.op == 123, 400);
    __expect_type(v2, "MaybeJust<Maybe<int8>>");
    __expect_type(v2.value, "MaybeNothing");

    return true;
}

@method_id(215)
fun test_IntAndRestInlineCell() {
    run<IntAndRestInlineCell>({ op: 123, rest: generateSlice_44_with_ref45() }, stringHexToSlice("0000007B0000002C").appendRef(stringHexToSlice("0000002D")));
    return true;
}

@method_id(216)
fun test_IntAndRestRefCell() {
    run<IntAndRestRefCell>({ op: 123, rest: generateCell_44_with_ref45() }, stringHexToSlice("0000007B").appendRef(stringHexToSlice("0000002C").appendRef(stringHexToSlice("0000002D"))));
    return true;
}

@method_id(217)
fun test_IntAndRestEitherCellOrRefCell() {
    val input1: IntAndRestEitherCellOrRefCell = { op: 123, rest: generateCell_44_with_ref45() };
    var s1 = input1.toCell().beginParse();
    s1.assertEqDeeply(stringHexToSlice("0000007BC_").appendRef(stringHexToSlice("0000002C").appendRef(stringHexToSlice("0000002D"))));
    match (val rest = IntAndRestEitherCellOrRefCell.fromSlice(s1).rest) {
        cell => assert_slice_is_44_and_ref45(rest.beginParse()),
        RemainingBitsAndRefs => assert_slice_is_44_and_ref45(rest)
    }

    val input2: IntAndRestEitherCellOrRefCell = { op: 123, rest: generateSlice_44_with_ref45() };
    var s2 = input2.toCell().beginParse();
    s2.assertEqDeeply(stringHexToSlice("0000007B000000164_").appendRef(stringHexToSlice("0000002D")));
    match (val rest = IntAndRestEitherCellOrRefCell.fromSlice(s2).rest) {
        cell => assert_slice_is_44_and_ref45(rest.beginParse()),
        RemainingBitsAndRefs => assert_slice_is_44_and_ref45(rest)
    }

    return true;
}

@method_id(218)
fun test_DifferentMaybeRefs() {
    run<DifferentMaybeRefs>({ op: 123, ref1m: null, ref2m: null, ref3: generateCell_44_with_ref45(), ref4m32: null, query_id: 456}, stringHexToSlice("0000007B00000000000000391_").appendRef(stringHexToSlice("0000002C").appendRef(stringHexToSlice("0000002D"))));
    run<DifferentMaybeRefs>({ op: 123, ref1m: generateCell_44_with_ref45(), ref2m: null, ref3: generateCell_44_with_ref45(), ref4m32: JustInt32{value: 999}.toCell(), query_id: 456}, stringHexToSlice("0000007BA0000000000000391_").appendRef(stringHexToSlice("0000002C").appendRef(stringHexToSlice("0000002D"))).appendRef(stringHexToSlice("0000002C").appendRef(stringHexToSlice("0000002D"))).appendRef(stringHexToSlice("000003E7")));
    run<DifferentMaybeRefs>({ op: 123, ref1m: null, ref2m: beginCell().endCell(), ref3: generateCell_44_with_ref45(), ref4m32: JustInt32{value: 998}.toCell(), query_id: 456}, stringHexToSlice("0000007B60000000000000391_").appendRef("").appendRef(stringHexToSlice("0000002C").appendRef(stringHexToSlice("0000002D"))).appendRef(stringHexToSlice("000003E6")));
    run<DifferentMaybeRefs>({ op: 123, ref1m: generateCell_44_with_ref45(), ref2m: generateCell_44_with_ref45(), ref3: beginCell().endCell(), ref4m32: null, query_id: 456}, stringHexToSlice("0000007BC0000000000000391_").appendRef(stringHexToSlice("0000002C").appendRef(stringHexToSlice("0000002D"))).appendRef(stringHexToSlice("0000002C").appendRef(stringHexToSlice("0000002D"))).appendRef(""));
    run<DifferentMaybeRefs>({ op: 123, ref1m: generateCell_44_with_ref45(), ref2m: beginCell().endCell(), ref3: generateCell_44_with_ref45(), ref4m32: JustInt32{value: 997}.toCell(), query_id: 456}, stringHexToSlice("0000007BE0000000000000391_").appendRef(stringHexToSlice("0000002C").appendRef(stringHexToSlice("0000002D"))).appendRef("").appendRef(stringHexToSlice("0000002C").appendRef(stringHexToSlice("0000002D"))).appendRef(stringHexToSlice("000003E5")));
    return true;
}

@method_id(219)
fun test_DifferentIntsWithMaybe() {
    run<DifferentIntsWithMaybe>({ ji: { value: 44 }, jmi: { value: 45 }, jiMaybe: null, jmiMaybe: null }, stringHexToSlice("0000002C800000169_"));
    run<DifferentIntsWithMaybe>({ ji: { value: 44 }, jmi: { value: null }, jiMaybe: { value: 45 }, jmiMaybe: { value: null } }, stringHexToSlice("0000002C4000000B6"));
    run<DifferentIntsWithMaybe>({ ji: { value: 44 }, jmi: { value: null }, jiMaybe: null, jmiMaybe: { value: 46 } }, stringHexToSlice("0000002C30000002E"));
    return DifferentIntsWithMaybe.fromSlice(stringHexToSlice("0000002C30000002E"));
}

@method_id(220)
fun test_DifferentMix1() {
    run<DifferentMix1>({ ja1: { addr: address("EQCRDM9h4k3UJdOePPuyX40mCgA4vxge5Dc5vjBR8djbEKC5") }, ja2m: { addr: address("EQCRDM9h4k3UJdOePPuyX40mCgA4vxge5Dc5vjBR8djbEKC5") }, ext_nn: makeExternalAddress(1234,30), imm: { op: 78, value: MaybeJust { value: MaybeJust { value: 78 } } }, tis: { op: 123, query_id: 889128, addr_e: makeExternalAddress(1234, 80) } },
        stringHexToSlice("80122199EC3C49BA84BA73C79F764BF1A4C1400717E303DC86E737C60A3E3B1B62180122199EC3C49BA84BA73C79F764BF1A4C1400717E303DC86E737C60A3E3B1B62087800004D2D3800000138000001ED2800000000000000000269000000000006C8944_"));
    run<DifferentMix1>({ ja1: { addr: address("EQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAM9c") }, ja2m: null, ext_nn: makeExternalAddress(0,3), imm: { op: 99, value: MaybeJust { value: MaybeJust { value: 99 } } }, tis: { op: 1234, query_id: 889129, addr_e: createAddressNone() } },
        stringHexToSlice("800000000000000000000000000000000000000000000000000000000000000000040636300000063000004D2000000000003644A6_"));

    val o = DifferentMix1.fromSlice(stringHexToSlice("800000000000000000000000000000000000000000000000000000000000000000040636300000063000004D2000000000003644A6_"));
    return (o.ja1.addr.getWorkchainAndHash(), o.ja2m, o.imm, o.tis.op, o.tis.addr_e.isNone() ? null : 0, o.tis.query_id, (o.ext_nn as slice).remainingBitsCount());
}

@method_id(221)
fun test_DifferentMix2() {
    run<DifferentMix2>({ iae: IntAndEither32OrRef64{ op: 777, i32orRef: 2983, query_id_maybe_ref: null }.toCell(), tic: { op: 123, amount: 829290000 }, rest: generateSlice_44_with_ref45() },
        stringHexToSlice("0000007B4316DF6100000002C").appendRef(stringHexToSlice("00000309000005D3A_")).appendRef(stringHexToSlice("0000002D")));
    val r1 = DifferentMix2.fromSlice(stringHexToSlice("0000007B4316DF6100000002C").appendRef(stringHexToSlice("00000309000005D3A_")).appendRef(stringHexToSlice("0000002D")));
    val iae1 = r1.iae.load();
    assert(iae1.i32orRef is int32, 400);
    assert(iae1.i32orRef == 2983, 400);
    assert(iae1.query_id_maybe_ref == null, 400);
    assert_slice_is_44_and_ref45(r1.rest);

    run<DifferentMix2>({ iae: { tvmCell: IntAndEither32OrRef64{ op: 778, i32orRef: { tvmCell: Inner2{ i64_in_ref: 9919992 }.toCell() }, query_id_maybe_ref: Inner1{ query_id_ref: 889477 }.toCell() }.toCell() }, tic: { op: 123, amount: 500000 }, rest: beginCell().endCell().beginParse() },
        stringHexToSlice("0000007B307A120").appendRef(stringHexToSlice("0000030AE_").appendRef(stringHexToSlice("0000000000975DF8")).appendRef(stringHexToSlice("00000000000D9285"))));
    var r2 = DifferentMix2.fromSlice(stringHexToSlice("0000007B307A120").appendRef(stringHexToSlice("0000030AE_").appendRef(stringHexToSlice("0000000000975DF8")).appendRef(stringHexToSlice("00000000000D9285"))));
    val iae2 = r2.iae.load();
    assert(iae2.i32orRef is Cell, 400);
    __expect_type(iae2.i32orRef.load(), "Inner2");
    assert(iae2.i32orRef.load().i64_in_ref == 9919992, 400);
    assert(iae2.query_id_maybe_ref != null, 400);
    __expect_type(iae2.query_id_maybe_ref.load(), "Inner1");
    assert(iae2.query_id_maybe_ref.load().query_id_ref == 889477, 400);
    assert(r2.tic.amount == 500000, 400);
    r2.rest.assertEnd();

    return true;
}

@method_id(222)
fun test_DifferentMix3() {
    run<DifferentMix3>({ bod: TwoInts32AndCoins{op:123, amount:80000}.toCell(), tim: {op: 456, amount:0}, pairm: null }, stringHexToSlice("4000007201_").appendRef(stringHexToSlice("0000007B3013880")));
    run<DifferentMix3>({ bod: TwoInts32AndCoins{op:124, amount:10}.toCell(), tim: null, pairm: (100000,100000) }, stringHexToSlice("200030D400000000000030D41_").appendRef(stringHexToSlice("0000007C10A")));
    run<DifferentMix3>({ bod: JustInt32{value:255}.toCell(), tim: null, pairm: (90,90) }, stringHexToSlice("A000000B400000000000000B5_").appendRef(stringHexToSlice("000000FF")));
    run<DifferentMix3>({ bod: JustInt32{value:510}.toCell(), tim: {op: 567, amount:9392843922}, pairm: (81923,81923) }, stringHexToSlice("C000008DD408BF6DB24A000280060000000000028007_").appendRef(stringHexToSlice("000001FE")));

    val o4 = DifferentMix3.fromSlice(stringHexToSlice("C000008DD408BF6DB24A000280060000000000028007_").appendRef(stringHexToSlice("000001FE")));
    val o2 = DifferentMix3.fromSlice(stringHexToSlice("200030D400000000000030D41_").appendRef(stringHexToSlice("0000007C10A")));
    return (
        (o4.bod is Cell<JustInt32>) ? o4.bod.load().value as int : -1, o4.tim, o4.pairm,
        777,
        o2.bod is Cell<JustInt32>, o2.bod is Cell<TwoInts32AndCoins>, o2.tim, o2.pairm,
    );
}

@method_id(223)
fun test_WriteWithBuilderReadWithOther() {
    var b = beginCell().storeUint(55, 32).storeMaybeRef(null);
    var w: WriteWithBuilder = { f1: 10, rest: b };
    return ReadWrittenWithBuilder.fromCell(w.toCell());
}

@method_id(224)
fun test_RestIsBuilderOrRemaining() {
    var remainingB = beginCell()
        .storeUint(5, 32)
        .storeAddress(address("9:527964d55cfa6eb731f4bfc07e9d025098097ef8505519e853986279bd8400d8"))
        .storeMaybeRef(JustInt32 { value: 123 }.toCell())
        .storeRef(JustAddress { addr: createAddressNone() }.toCell());
    var w: ReadWriteRest<builder> = {
        f1: 60,
        f2: ton("0.05"),
        rest: remainingB
    };
    var r: ReadRest_Remaining = w.toCell().beginParse().loadAny();
    __expect_type(r.rest, "RemainingBitsAndRefs");
    var rest = Tail224.fromSlice(r.rest);
    return (r.f1, r.f2, rest.ji.value, rest.addr.getWorkchain(), rest.ref1!.load(), rest.ref2.load().addr.isNone());
}

@method_id(225)
fun test_MidIsBuilderOrBitsN() {
    var bits40_b = beginCell().storeSlice(stringHexToSlice("0000FFFF00"));
    var typedCell = ReadWriteMid {
        f1: 5,
        mid: bits40_b,
        f3: ton("0.05")
    }.toCell();
    __expect_type(typedCell, "Cell<ReadWriteMid<builder>>");
    var r = ReadWriteMid<bits40>.fromCell(typedCell);
    var mid = r.mid as slice;
    return (r.f1, mid.remainingBitsCount(), mid.loadAny<int32>(), mid.remainingBitsCount(), r.f3);
}


fun main() {
    var t: JustInt32 = { value: 10 };
    var c = t.toCell();
    var t2 = c.load();
    var t3 = JustInt32.fromSlice(c.beginParse());
    return (t2.value, t3.value);
}

/**
@testcase | 0   | | 10 10
@testcase | 200 | | BC{00100000007b000001c8}
@testcase | 201 | | 123 456
@testcase | 202 | | -1
@testcase | 203 | | -1
@testcase | 204 | | -1
@testcase | 205 | | -1
@testcase | 206 | | -1
@testcase | 207 | | -1
@testcase | 208 | | -1
@testcase | 209 | | -1
@testcase | 210 | | 123 555 46 (null)
@testcase | 211 | | (null) 132 123
@testcase | 212 | | (null) 133 123
@testcase | 213 | | -1
@testcase | 214 | | -1
@testcase | 215 | | -1
@testcase | 216 | | -1
@testcase | 217 | | -1
@testcase | 218 | | -1
@testcase | 219 | | 44 (null) (null) 46 143
@testcase | 220 | | 0 0 (null) 99 136 137 99 1234 (null) 889129 14
@testcase | 221 | | -1
@testcase | 222 | | 510 567 9392843922 146 81923 81923 147 777 0 -1 (null) (null) 0 100000 100000 147
@testcase | 223 | | 10 55 (null)
@testcase | 224 | | 60 50000000 5 9 123 -1
@testcase | 225 | | 5 40 65535 8 50000000
 */
