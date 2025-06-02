
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
fun slice.appendRef(self, refSlice: slice): slice {
    return beginCell().storeSlice(self).storeRef(beginCell().storeSlice(refSlice).endCell()).endCell().beginParse();
}

@inline
fun generateSlice_44_with_ref45(): slice {
    return generateCell_44_with_ref45().beginParse();
}

@inline
fun generateCell_44_with_ref45(): cell {
    return beginCell().storeInt(44, 32).storeRef(beginCell().storeInt(45, 32).endCell()).endCell();
}


fun run<TInputStruct>(input: TInputStruct, ans: slice) {
    repeat (2) {
        var s = input.toCell().beginParse();
        input = TInputStruct.fromSlice(s.assertEqDeeply(ans));
    }
    input.toCell().beginParse().skipAny<TInputStruct>().assertEnd();
}


/*
single_prefix32#87654321
    amount1:Grams
    amount2:Grams
    = MsgSinglePrefix32;
*/

struct(0x87654321) MsgSinglePrefix32 {
    amount1: coins;
    amount2: coins;
}

/*
single_prefix48#876543211234
    amount:(Either Grams uint64)
    = MsgSinglePrefix48;
*/

struct(0x876543211234) MsgSinglePrefix48 {
    amount: coins | uint64;
}

/*
counterIncrement#12345678
    counter_id:int8
    inc_by:int32
    = MsgCounter1;
*/

struct(0x12345678) CounterIncrement {
    counter_id: int8;
    inc_by: int32;
}

/*
counterDecrement#23456789
    counter_id:int8
    dec_by:int32
    = MsgCounter1;
*/

struct(0x23456789) CounterDecrement {
    counter_id: int8;
    dec_by: int32;
}

/*
counterReset0#34567890
    counter_id:int8
    = MsgCounter1;
*/

struct(0x34567890) CounterReset0 {
    counter_id: int8;
}

/*
counterResetTo#00184300
    counter_id:int8
    initial_value:int64
    = MsgCounter1;
*/

struct(0x00184300) CounterResetTo {
    counter_id: int8;
    initial_value: int64;
}

type MsgCounter1 =
    CounterIncrement |
    CounterDecrement |
    CounterReset0 |
    CounterResetTo |
;

/*
bodyPayload1$001
    should_forward:Bool
    n_times:int32
    content:Cell
    = BodyPayload;
*/

struct(0b001) BodyPayload1 {
    should_forward: bool;
    n_times: int32;
    content: RemainingBitsAndRefs;
}

/*
bodyPayload2$01
    master_id:int8
    owner_address:MsgAddressInt
    = BodyPayload;
*/

struct(0b01) BodyPayload2 {
    master_id: int8;
    owner_address: address;
}

type BodyPayload = BodyPayload1 | BodyPayload2;

/*
sayHiAndGoodbye#89
    dest_addr:(Maybe MsgAddressInt)
    body:BodyPayload
    = MsgExternal1;
*/

struct(0x89) SayHiAndGoodbye {
    dest_addr: address?;
    body: BodyPayload;
}

/*
sayStoreInChain#0013
    in_masterchain:Bool
    contents:^BodyPayload
    = MsgExternal1;
 */

struct(0x0013) SayStoreInChain {
    in_masterchain: bool;
    contents: Cell<BodyPayload>;
}

type MsgExternal1 = SayHiAndGoodbye | SayStoreInChain;

/*
transferParams1#794
    dest_int:MsgAddressInt
    amount:Grams
    dest_ext:MsgAddressExt
    = TransferParams;
*/

struct(0x794) TransferParams1 {
    dest_int: address;
    amount: coins;
    dest_ext: address;
}

/*
transferParams2#9
    intVector:(Both int32 (Both (Maybe Grams) uint64))
    needs_more:^Bit
    = TransferParams;
 */

struct(0x9) TransferParams2 {
    intVector: (int32, coins?, uint64);
    needs_more: Cell<bool>;
}

type TransferParams = TransferParams1 | TransferParams2;

/*
_#FB3701FF
    params:(Either TransferParams ^TransferParams)
    = MsgTransfer;
 */

struct(0xFB3701FF) MsgTransfer {
    params: Either<TransferParams, Cell<TransferParams>>;
}



// ---------------------------------------------


@method_id(201)
fun test_MsgSinglePrefix32() {
    run<MsgSinglePrefix32>({ amount1: 80, amount2: 800000000 }, stringHexToSlice("8765432115042FAF0800"));

    return MsgSinglePrefix32.fromSlice(stringHexToSlice("8765432115042FAF0800"));
}

@method_id(202)
fun test_MsgSinglePrefix48() {
    run<MsgSinglePrefix48>({ amount: 80 as uint64 }, stringHexToSlice("87654321123480000000000000284_"));
    run<MsgSinglePrefix48>({ amount: 800000000 as coins }, stringHexToSlice("876543211234217D784004_"));

    var o = MsgSinglePrefix48.fromSlice(stringHexToSlice("876543211234217D784004_"));
    return (o, 777, o.amount is coins, o.amount is uint64);
}

@method_id(203)
fun test_MsgCounter1() {
    run<MsgCounter1>(CounterIncrement{ counter_id: 123, inc_by: 78 }, stringHexToSlice("123456787B0000004E"));
    run<MsgCounter1>(CounterDecrement{ counter_id: 0, dec_by: -38 }, stringHexToSlice("2345678900FFFFFFDA"));
    run<MsgCounter1>(CounterReset0{ counter_id: 0 }, stringHexToSlice("3456789000"));
    run<MsgCounter1>(CounterResetTo{ counter_id: 0, initial_value: 29874329774732 }, stringHexToSlice("001843000000001B2BA8D06A8C"));

    val o = MsgCounter1.fromSlice(stringHexToSlice("3456789000"));
    return (o, 777, o is CounterReset0, o is CounterIncrement);
}

@method_id(204)
fun test_MsgExternal1() {
    run<MsgExternal1>(SayHiAndGoodbye{ dest_addr: null, body: BodyPayload2 { master_id: 10, owner_address: address("EQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAM9c") } }, stringHexToSlice("892150000000000000000000000000000000000000000000000000000000000000000002_"));
    run<MsgExternal1>(SayHiAndGoodbye{ dest_addr: address("EQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAM9c"), body: BodyPayload1 { should_forward: false, n_times: 85, content: generateSlice_44_with_ref45() } }, stringHexToSlice("89C0000000000000000000000000000000000000000000000000000000000000000002000000550000002C").appendRef(stringHexToSlice("0000002D")));
    run<MsgExternal1>(SayHiAndGoodbye{ dest_addr: address("Ef8o6AM9sUZ8rOqLFY8PYeaC3gbopZR1BMkE8fcD0r5NnmCi"), body: BodyPayload2 { master_id: -5, owner_address: address("Ef8o6AM9sUZ8rOqLFY8PYeaC3gbopZR1BMkE8fcD0r5NnmCi") } }, stringHexToSlice("89CFF28E8033DB1467CACEA8B158F0F61E682DE06E8A5947504C904F1F703D2BE4D9E7EE7F9474019ED8A33E5675458AC787B0F3416F037452CA3A82648278FB81E95F26CF4_"));
    run<MsgExternal1>(SayStoreInChain{ in_masterchain: true, contents: { tvmCell: BodyPayload1{ should_forward: true, n_times: 20, content: generateSlice_44_with_ref45() }.toCell()} }, stringHexToSlice("0013C_").appendRef(stringHexToSlice("3000000140000002C").appendRef(stringHexToSlice("0000002D"))));
    run<MsgExternal1>(SayStoreInChain{ in_masterchain: false, contents: { tvmCell: BodyPayload2{ master_id: 37, owner_address: address(("EQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAM9c")) }.toCell()} }, stringHexToSlice("00134_").appendRef(stringHexToSlice("4960000000000000000000000000000000000000000000000000000000000000000004_")));

    val o = MsgExternal1.fromSlice(stringHexToSlice("00134_").appendRef(stringHexToSlice("4960000000000000000000000000000000000000000000000000000000000000000004_")));
    assert(o is SayStoreInChain, 400);
    val contents = o.contents.load();
    return (contents is BodyPayload2 && contents.owner_address == address("EQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAM9c"), contents is BodyPayload1, contents is BodyPayload2);
}

@method_id(205)
fun test_MsgTransfer() {
    run<MsgTransfer>({ params: EitherLeft { value: TransferParams1 { dest_int: address("Ef8o6AM9sUZ8rOqLFY8PYeaC3gbopZR1BMkE8fcD0r5NnmCi"), amount: 80000000, dest_ext: makeExternalAddress(1234,80) } } }, stringHexToSlice("FB3701FF3CA4FF28E8033DB1467CACEA8B158F0F61E682DE06E8A5947504C904F1F703D2BE4D9E404C4B4004A0000000000000000009A5_"));
    run<MsgTransfer>({ params: EitherLeft { value: TransferParams2 { intVector: (123, 1234567890123456, 1234567890123456), needs_more: {tvmCell: beginCell().storeBool(true).endCell()} } } }, stringHexToSlice("FB3701FF48000003DDC118B54F22AEB0000118B54F22AEB02_").appendRef(stringHexToSlice("C_")));
    run<MsgTransfer>({ params: EitherRight { value: { tvmCell: TransferParams1{ dest_int: address("EQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAM9c"), amount: 80000000, dest_ext: makeExternalAddress(1234,70) }.toCell()} } }, stringHexToSlice("FB3701FFC_").appendRef(stringHexToSlice("7948000000000000000000000000000000000000000000000000000000000000000000809896800918000000000000004D2")));
    run<MsgTransfer>({ params: EitherRight { value: { tvmCell: TransferParams2{ intVector: (123, null, 0), needs_more: {tvmCell: beginCell().storeBool(false).endCell()} }.toCell()} } }, stringHexToSlice("FB3701FFC_").appendRef(stringHexToSlice("90000007B00000000000000004_").appendRef(stringHexToSlice("4_"))));

    val o = MsgTransfer.fromSlice(stringHexToSlice("FB3701FFC_").appendRef(stringHexToSlice("90000007B00000000000000004_").appendRef(stringHexToSlice("4_"))));
    return (
        o.params is EitherLeft, o.params is EitherRight,
        o.params is EitherRight && TransferParams.fromCell(o.params.value.tvmCell) is TransferParams1,
        o.params is EitherRight && TransferParams.fromCell(o.params.value.tvmCell) is TransferParams2,
    );
}


fun main() {}

/**
@testcase | 201 | | 80 800000000
@testcase | 202 | | 800000000 17 777 -1 0
@testcase | 203 | | (null) 0 131 777 -1 0
@testcase | 204 | | -1 0 -1
@testcase | 205 | | 0 -1 0 -1
 */
