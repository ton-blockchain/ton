import "@stdlib/tvm-dicts"

/*
int_msg_info$0                                                              1
    ihr_disabled:Bool                   // always 0, not implemented        1
    bounce:Bool                         // parameter                        1
    bounced:Bool                        // always 0 on send                 1
    src:MsgAddress                      // always 00 on send                2
    dest:MsgAddressInt                  // parameter                        267
    value:CurrencyCollection            // parameter                        124 + 1
    extra_flags:Grams                   // if new bounces enabled           12
    fwd_fee:Grams                       // always 0 on send                 4
    created_lt:uint64                   // always 0 on send                 64
    created_at:uint32                   // always 0 on send                 32
    = CommonMsgInfoRelaxed;
_   split_depth:(Maybe (## 5))                                              1 + 5
    special:(Maybe TickTock)                                                1 + 2
    code:(Maybe ^Cell)                                                      1
    data:(Maybe ^Cell)                                                      1
    library:(Maybe ^Cell)                                                   1
    = StateInit;
message$_ {X:Type}
    info:CommonMsgInfoRelaxed                                               510
    init:(Maybe (Either StateInit ^StateInit))                              12
    body:(Either X ^X)                  // body is either embedded or stored as ref
    = MessageRelaxed X;
 */

fun getMyAddressDev(): address
    asm "x{80194DC6438F99D3D9DBE151944925D90B2492954BF6B9C070FBFF2DDED5F30547D_} PUSHSLICE";

@inline_ref
fun calculateNftItemStateInitData(itemIndex: int): cell {
    return beginCell()
            .storeUint(itemIndex, 64)
            .storeAddress(getMyAddressDev())
            .endCell();
}

@inline
fun calculateStateInitCell(code: cell, data: cell): cell {
    return beginCell()
        .storeUint(0, 2)        // 0 split_depth, 0 special
        .storeDict(code)
        .storeDict(data)
        .storeUint(0, 1)        // 0 library
        .endCell();
}

fun calculateNftItemAddress(workchain: int, stateInitCell: cell): address {
    return beginCell()
        .storeUint(0b100, 3)            // addr_std$10 + 0 split_depth
        .storeInt(workchain, 8)
        .storeUint(stateInitCell.hash(), 256)
        .endCell()
        .beginParse() as address;
}

struct(0x12345678) MyBody {
    queryId: uint64;
}

@noinline
fun test1_manual() {
    return beginCell()
        .storeUint(0x18, 6)            // bounce
        .storeAddress(address("UQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPuwA"))
        .storeCoins(123)   // value.grams
        .storeUint(0, 1 + 4 + 4 + 64 + 32 + 1 + 1)  // ... + 0 init + 0 body (not ref)
        // MyBody: op + queryId
        .storeUint(0x12345678, 32)
        .storeUint(800, 64)
        .endCell();
}

@method_id(101)
fun test1() {
    val body: MyBody = { queryId: 800 };
    var b = createMessage({
        body: body,
        bounce: BounceMode.Only256BitsOfBody,
        dest: address("UQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPuwA"),
        value: 123,
    });
    assert(b.hash() == test1_manual().hash(), 101);
    return b.hash();
}

@noinline
fun test2_manual() {
    return beginCell()
        .storeUint(0x10, 6)            // no bounce
        .storeAddress(address("1:527964d55cfa6eb731f4bfc07e9d025098097ef8505519e853986279bd8400d8"))
        .storeCoins(90)   // value.grams
        .storeUint(0, 1 + 4 + 4 + 64 + 32 + 1 + 1)  // ... + 0 init + 0 body (not ref)
        // body: op + queryId
        .storeUint(0x12345678, 32)
        .storeUint(127493264572, 64)
        .endCell();
}

@method_id(102)
fun test2() {
    val body: MyBody = { queryId: 127493264572 };
    var b = createMessage({
        body,
        bounce: BounceMode.NoBounce,
        dest: address("1:527964d55cfa6eb731f4bfc07e9d025098097ef8505519e853986279bd8400d8"),
        value: 90,
    });
    assert(b.hash() == test2_manual().hash(), 102);
    return b.hash();
}

@noinline
fun test3_manual() {
    val body_ref = beginCell()
        .storeUint(0x12345678, 32)
        .storeUint(127493264572, 64)
        .endCell();
    return beginCell()
        .storeUint(0x10, 6)            // no bounce
        .storeAddress(address("1:527964d55cfa6eb731f4bfc07e9d025098097ef8505519e853986279bd8400d8"))
        .storeCoins(90)   // value.grams
        .storeUint(1, 1 + 4 + 4 + 64 + 32 + 1 + 1)  // ... + 0 init + 1 body (ref)
        .storeRef(body_ref)
        .endCell();
}

@method_id(103)
fun test3() {
    var b = createMessage({
        body: MyBody{ queryId: 127493264572 }.toCell(),
        bounce: false,
        dest: address("1:527964d55cfa6eb731f4bfc07e9d025098097ef8505519e853986279bd8400d8"),
        value: 90,
    });
    assert(b.hash() == test3_manual().hash(), 103);
    return b.hash();
}

@noinline
fun test4_manual(bodyCell: cell, dest: address, value: coins) {
    return beginCell()
        .storeUint(0x18, 6)            // bounce
        .storeAddress(dest)
        .storeCoins(value)
        .storeUint(1, 1 + 4 + 4 + 64 + 32 + 1 + 1)  // ... + 0 init + 1 body (ref)
        .storeRef(bodyCell)
        .endCell();
}

@method_id(104)
fun test4(value: coins) {
    val bodyCell = beginCell()
        .storeUint(0x03738FA9, 32)
        .storeBool(true).storeBool(false).storeCoins(123)
        .endCell();
    val dest = address("0:FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFfffffffffffffffffffffffffffff");
    var b = createMessage({
        body: bodyCell,
        value,
        bounce: true,
        dest: dest,
    });
    assert(b.hash() == test4_manual(bodyCell, dest, value).hash(), 104);
    return b.hash();
}

@noinline
fun test5_manual() {
    var ec_dict: ExtraCurrenciesMap = createEmptyMap();
    ec_dict.set(1, 100);
    return beginCell()
        .storeUint(0x18, 6)            // bounce
        .storeAddress(address("UQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPuwA"))
        .storeCoins(123)          // value.grams
        .storeMaybeRef(ec_dict.toLowLevelDict())   // value.extra
        .storeUint(0, 0 + 4 + 4 + 64 + 32 + 1 + 1)  // ... + 0 init + 0 body (not ref)
        // body: op + queryId
        .storeUint(0x12345678, 32)
        .storeUint(800, 64)
        .endCell();
}

@method_id(105)
fun test5() {
    var ec_dict: ExtraCurrenciesMap = createEmptyMap();
    ec_dict.set(1, 100);
    val body: MyBody = { queryId: 800 };
    var b = createMessage({
        body,
        bounce: BounceMode.Only256BitsOfBody,
        dest: address("UQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPuwA"),
        value: (123, ec_dict),
    });
    assert(b.hash() == test5_manual().hash(), 105);
    return b.hash();
}

@noinline
fun test6_manual(value: coins, ec_dict: dict) {
    return beginCell()
        .storeUint(0x18, 6)            // bounce
        .storeAddress(address("UQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPuwA"))
        .storeCoins(value)        // value.grams
        .storeMaybeRef(ec_dict)   // value.extra
        .storeUint(0, 0 + 4 + 4 + 64 + 32 + 1 + 1)  // ... + 0 init + 0 body (not ref)
        // body: op + queryId
        .storeUint(0x12345678, 32)
        .storeUint(800, 64)
        .endCell();
}

@method_id(106)
fun test6(value: coins, dictKey: int?) {
    var ec_dict: ExtraCurrenciesMap = createEmptyMap();
    if (dictKey != null) {
        ec_dict.set(dictKey, 100500);
    }
    val body: MyBody = { queryId: 800 };
    var b = createMessage({
        bounce: true,
        dest: address("UQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPuwA"),
        value: (value, ec_dict),
        body,
    });
    assert(b.hash() == test6_manual(value, ec_dict.toLowLevelDict()).hash(), 106);
    return b.hash();
}

struct MyNftBody {
    nftContent: cell;
}

@noinline
fun test7_manual(nftItemCode: cell, amount: coins) {
    var (itemIndex: int, nftContent: cell) = (10, beginCell().endCell());
    val nftItemData = calculateNftItemStateInitData(itemIndex);
    val nftAddress = calculateNftItemAddress(BASECHAIN, calculateStateInitCell(nftItemCode, nftItemData));

    return beginCell()
        .storeUint(0x18, 6)         // bounce
        .storeAddress(nftAddress)
        .storeCoins(amount)
        // 1 state init exists, 0 either left (state init embedded), 00110 (code and data), 0 either left (body inline)
        .storeUint(0b10001100, 1 + 4 + 4 + 64 + 32 + (1 + 1 + 5 + 1))
        .storeRef(nftItemCode)
        .storeRef(nftItemData)
        .storeRef(nftContent)
        .endCell();
}

@method_id(107)
fun test7(amount: coins) {
    val nftItemCode: cell = beginCell().storeInt(0x273849723892, 94).endCell();
    var (itemIndex: int, nftContent: cell) = (10, beginCell().endCell());
    val stateInitData = calculateNftItemStateInitData(itemIndex);

    val body: MyNftBody = { nftContent };
    var b = createMessage({
        bounce: true,
        body,
        dest: { workchain: BASECHAIN, stateInit: { code: nftItemCode, data: stateInitData } },
        value: amount,
    });
    assert(b.hash() == test7_manual(nftItemCode, amount).hash(), 107);
    return b.hash();
}

struct(0x706c7567) RequestPaymentMessage {
    queryId: uint64;
    amount: coins;
    someDict: dict;
}

@noinline
fun test8_manual(destAddr: address, requestedAmount: coins) {
    return beginCell()
        .storeUint(0x18, 6)            // bounce
        .storeAddress(destAddr)
        .storeCoins(123)   // value.grams
        .storeUint(0, 1 + 4 + 4 + 64 + 32 + 1 + 1)  // ... + 0 init + 0 body (not ref)
        .storeUint(0x706c7567, 32)
        .storeUint(0, 64)
        .storeCoins(requestedAmount)
        .storeDict(null)
        .endCell();
}

@method_id(108)
fun test8(requestedAmount: coins) {
    val destAddr = address("Ef9nzj6RBc4mQ6p3ng7mGJ7tp7MbzERhe7obkM9A0wnCCEcf");
    var b = createMessage({
        bounce: true,
        dest: destAddr,
        value: 123,
        body: RequestPaymentMessage {
            queryId: 0,
            amount: requestedAmount,
            someDict: null,
        }
    });
    assert(b.hash() == test8_manual(destAddr, requestedAmount).hash(), 108);
    return b.hash();
}

@noinline
fun test9_manual(bounceable: bool) {
    return beginCell()
        .storeUint(0b01, 2).storeBool(bounceable).storeUint(0b000, 3)
        .storeAddress(address("UQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPuwA"))
        .storeCoins(0)   // value.grams
        .storeUint(0, 1 + 4 + 4 + 64 + 32 + 1 + 1)  // ... + 0 init + 0 body (not ref)
        .storeUint(0x12345678, 32)
        .storeUint(800, 64)
        .storeInt(-777, 13)
        .endCell();
}

@method_id(109)
fun test9(bounceable: bool) {
    var b = createMessage({
        bounce: bounceable,
        dest: address("UQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPuwA"),
        value: 0,
        body: (0x12345678 as uint32, 800 as uint64, -777 as int13),
    });
    assert(b.hash() == test9_manual(bounceable).hash(), 109);
    return b.hash();
}

type Body500Bits = (uint250, uint250);      // this body guaranteely fits into cell

@noinline
fun test10_manual(bd: Body500Bits) {
    return beginCell()
        .storeUint(0x10, 6)
        .storeAddress(address("Ef80FNJ5NJO4-0QwlVAWckUZXdk-PfYDexDZ1-ju9SxhF0A6"))
        .storeCoins(1 << 118)
        .storeUint(0, 1 + 4 + 4 + 64 + 32 + 1 + 1)  // ... + 0 init + 0 body (not ref)
        .storeUint(bd.0, 250)
        .storeUint(bd.1, 250)
        .endCell();
}

@method_id(110)
fun test10(bd: Body500Bits) {
    var b = createMessage({
        bounce: BounceMode.NoBounce,
        body: bd,
        dest: address("Ef80FNJ5NJO4-0QwlVAWckUZXdk-PfYDexDZ1-ju9SxhF0A6"),
        value: 1 << 118,
    });
    assert(b.hash() == test10_manual(bd).hash(), 110);
    return b.hash();
}

type Body750Bits = (uint250, uint250, uint250);      // this body is auto-ref

@noinline
fun test11_manual(bd: Body750Bits) {
    val bodyRef = beginCell().storeUint(bd.0,250).storeUint(bd.1,250).storeUint(bd.2,250).endCell();
    return beginCell()
        .storeUint(0x10, 6)
        .storeAddress(address("Ef80FNJ5NJO4-0QwlVAWckUZXdk-PfYDexDZ1-ju9SxhF0A6"))
        .storeCoins(1 << 110)
        .storeUint(1, 1 + 4 + 4 + 64 + 32 + 1 + 1)  // ... + 0 init + 1 body (ref)
        .storeRef(bodyRef)
        .endCell();
}

@method_id(111)
fun test11(bd: Body750Bits) {
    var b = createMessage({
        bounce: BounceMode.NoBounce,
        body: bd,
        dest: address("Ef80FNJ5NJO4-0QwlVAWckUZXdk-PfYDexDZ1-ju9SxhF0A6"),
        value: 1 << 110,
    });
    assert(b.hash() == test11_manual(bd).hash(), 111);
    return b.hash();
}

@noinline
fun test12_manual(data32: uint32) {
    var init = ContractState {
        data: beginCell().storeUint(data32,32).endCell(),
        code: beginCell().endCell(),
    };

    return beginCell()
        .storeUint(0x18, 6)         // bounce
        .storeUint(0b100, 3).storeInt(MASTERCHAIN, 8).storeUint(StateInit.calcHashCodeData(init.code, init.data), 256)
        .storeCoins(ton("0.05"))
        // 1 state init exists, 0 either left (state init embedded), 00110 (code and data), 0 either left (body inline)
        .storeUint(0b10001100, 1 + 4 + 4 + 64 + 32 + (1 + 1 + 5 + 1))
        .storeRef(init.code)
        .storeRef(init.data)
        .storeUint(data32, 32)
        .endCell();
}

@method_id(112)
fun test12(data32: uint32) {
    var init = ContractState {
        data: beginCell().storeUint(data32,32).endCell(),
        code: beginCell().endCell(),
    };

    var b = createMessage({
        bounce: true,
        body: data32,
        dest: { workchain: MASTERCHAIN, stateInit: init },
        value: ton("0.05"),
    });
    assert(b.hash() == test12_manual(data32).hash(), 112);
    return b.hash();
}

@noinline
fun test13_manual() {
    return beginCell()
        .storeUint(0x18, 6)            // bounce
        .storeAddress(address("EQDE0HBgfkOiqHezLtExBGTvOs8eitthHQosBjW3BmDy1y2K"))
        .storeCoins(52 << 78)   // value.grams
        .storeUint(0, 1 + 4 + 4 + 64 + 32 + 1 + 1)  // ... + 0 init + 0 body (not ref)
        .endCell();     // no body at all
}

@method_id(113)
fun test13() {
    var b = createMessage({
        bounce: true,
        dest: address("EQDE0HBgfkOiqHezLtExBGTvOs8eitthHQosBjW3BmDy1y2K"),
        value: 52 << 78,
    });
    assert(b.hash() == test13_manual().hash(), 113);
    return b.hash();
}

@noinline
fun test14_manual(stateInitCell: cell, amount: coins) {
    val nftAddress = calculateNftItemAddress(BASECHAIN, stateInitCell);

    return beginCell()
        .storeUint(0x18, 6)         // bounce
        .storeAddress(nftAddress)
        .storeCoins(amount)
        // 1 state init exists, 1 either right (state init ref), 0 either left (body inline)
        .storeUint(0b110, 1 + 4 + 4 + 64 + 32 + (1 + 1 + 1))
        .storeRef(stateInitCell)
        .endCell();     // no body
}

@method_id(114)
fun test14(amount: coins) {
    val stateInitCode = beginCell().storeInt(0x273849723892, 94).endCell();
    val stateInitData = calculateNftItemStateInitData(10);
    val stateInitCell = calculateStateInitCell(stateInitCode, stateInitData);

    var b = createMessage({
        bounce: true,
        dest: { stateInit: stateInitCell },
        value: amount,
    });
    assert(b.hash() == test14_manual(stateInitCell, amount).hash(), 114);
    return b.hash();
}

struct(0x1234) Body15 {
    tens: Body750Bits;
    more: int32;
}

@noinline
fun test15_manual(stateInitCell: cell, bd: Body15) {
    val bodyRef = beginCell().storeUint(0x1234,16).storeUint(bd.tens.0,250).storeUint(bd.tens.1,250).storeUint(bd.tens.2,250).storeInt(bd.more,32).endCell();
    val nftAddress = calculateNftItemAddress(MASTERCHAIN, stateInitCell);

    return beginCell()
        .storeUint(0x10, 6)         // no bounce
        .storeAddress(nftAddress)
        .storeCoins(ton("100.0004"))
        // 1 state init exists, 1 either right (state init ref), 1 either right (body ref)
        .storeUint(0b111, 1 + 4 + 4 + 64 + 32 + (1 + 1 + 1))
        .storeRef(stateInitCell)
        .storeRef(bodyRef)
        .endCell();     // no body
}

@method_id(115)
fun test15(tens0: int, tens1: int) {
    val stateInitCode = beginCell().storeInt(0x273849723892, 94).endCell();
    val stateInitData = calculateNftItemStateInitData(10);
    val stateInitCell = beginCell()
        .storeUint(1, 1)        // has split_depth
        .storeUint(3, 5)        // split_depth
        .storeUint(0, 1)        // 0 special
        .storeDict(stateInitCode)
        .storeDict(stateInitData)
        .storeUint(0, 1)        // 0 library
        .endCell();
    var bd: Body15 = {
        tens: (1 << 88, tens0, tens1),
        more: max(tens0, tens1),
    };

    var b = createMessage({
        bounce: false,
        dest: { workchain: MASTERCHAIN, stateInit: stateInitCell },
        body: bd,
        value: ton("100.0004"),
    });
    assert(b.hash() == test15_manual(stateInitCell, bd).hash(), 115);
    return b.hash();
}

@noinline
fun test18_manual(dest: builder, queryId: uint64) {
    return beginCell()
        .storeUint(0x18, 6)            // bounce
        .storeBuilder(dest)
        .storeCoins(123)   // value.grams
        .storeUint(0, 1 + 4 + 4 + 64 + 32 + 1 + 1)  // ... + 0 init + 0 body (not ref)
        // MyBody: op + queryId
        .storeUint(0x12345678, 32)
        .storeUint(queryId, 64)
        .endCell();
}

@method_id(118)
fun test18(queryId: uint64) {
    val body: MyBody = { queryId };
    var b = createMessage({
        body: body,
        bounce: true,
        dest: beginCell().storeUint(0, 2),
        value: 123,
    });
    assert(b.hash() == test18_manual(beginCell().storeUint(0, 2), queryId).hash(), 118);
    return b.hash();
}

@noinline
fun test19_manual(bd: uint64) {
    var destB = beginCell().storeUint(0, 2);
    return beginCell()
        .storeUint(0x10, 6)         // no bounce
        .storeBuilder(destB)
        .storeCoins(ton("0.059"))
        .storeUint(0, 1 + 4 + 4 + 64 + 32 + 1 + 1)  // ... + 0 init + 0 body (not ref)
        .storeUint(bd, 64)
        .endCell();
}

@method_id(119)
fun test19(bd: uint64) {
    var destB = beginCell().storeUint(0, 2);
    var b = createMessage({
        bounce: 10 < 3,
        body: bd,
        dest: destB,
        value: ton("0.059"),
    });
    assert(b.hash() == test19_manual(bd).hash(), 119);
    return b.hash();
}

@noinline
fun test20_manual() {
    var bodyB = beginCell().storeUint(0x12345678, 32).storeUint(800, 64);
    return beginCell()
        .storeUint(0x18, 6)            // bounce
        .storeAddress(address("UQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPuwA"))
        .storeCoins(123)   // value.grams
        .storeUint(1, 1 + 4 + 4 + 64 + 32 + 1 + 1)  // ... + 0 init + 1 body (ref)
        .storeRef(bodyB.endCell())
        .endCell();
}

@method_id(120)
fun test20() {
    var bodyB = beginCell().storeUint(0x12345678, 32).storeUint(800, 64);
    var b = createMessage({
        body: bodyB,
        bounce: true,
        dest: address("UQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPuwA"),
        value: 123,
    });
    assert(b.hash() == test20_manual().hash(), 120);
    return b.hash();
}

struct(0x12345678) Body21 {
    queryId: uint64;
    payload: RemainingBitsAndRefs;
}

@noinline
fun test21_manual(bd: Body21) {
    return beginCell()
        .storeUint(0x10, 6)            // no bounce
        .storeAddress(address("EQAUzE-Nef80O9dLZy91HfPiOb6EEQ8YqyWKyIU-KeaYLNUi"))
        .storeCoins(ton("0.1"))     // value.grams
        .storeDict(createEmptyDict())
        .storeUint(1, 4 + 4 + 64 + 32 + 1 + 1)  // ... + 0 init + 1 body (ref)
        .storeRef(bd.toCell())      // stored as ref due to remainder of unpredictable size
        .endCell();
}

@method_id(121)
fun test21(queryId: uint64) {
    var payloadSlice = beginCell().storeUint(0x12345678, 32).storeUint(800, 64).storeRef(createEmptyCell()).endCell().beginParse();
    var body: Body21 = { queryId, payload: payloadSlice };
    var b = createMessage({
        bounce: false,
        dest: address("EQAUzE-Nef80O9dLZy91HfPiOb6EEQ8YqyWKyIU-KeaYLNUi"),
        value: (ton("0.1"), createEmptyMap<int32, varuint32>()),
        body,
    });
    assert(b.hash() == test21_manual(body).hash(), 121);
    return b.hash();
}

@noinline
fun test22_manual(unsafeB: builder) {
    return beginCell()
        .storeUint(0x18, 6)            // bounce
        .storeAddress(address("0:FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFfffffffffffffffffffffffffffff"))
        .storeCoins(ton("0.08"))
        .storeUint(0, 1 + 4 + 4 + 64 + 32 + 1 + 1)  // ... + 0 init + 0 body (no ref)
        .storeBuilder(unsafeB)
        .endCell();
}

@method_id(122)
fun test22(op: uint32, queryId: uint64) {
    var bodyB = beginCell().storeUint(op, 32).storeUint(queryId, 64);
    var b = createMessage({
        bounce: true,
        dest: address("0:FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFfffffffffffffffffffffffffffff"),
        value: ton("0.08"),
        body: UnsafeBodyNoRef {
            forceInline: bodyB
        },
    });
    assert(b.hash() == test22_manual(bodyB).hash(), 122);
    return b.hash();
}

struct Body23Unlimited {
    any1: (address?, builder),
    any2: RemainingBitsAndRefs;
}

@noinline
fun test23_manual(state32: uint32, body: Body23Unlimited) {
    var init = ContractState {
        data: beginCell().storeUint(state32,32).endCell(),
        code: beginCell().endCell(),
    };

    return beginCell()
        .storeUint(0x18, 6)         // bounce
        .storeUint(0b100, 3).storeInt(9, 8).storeUint(StateInit.calcHashCodeData(init.code, init.data), 256)
        .storeCoins(ton("0.10009"))
        // 1 state init exists, 0 either left (state init embedded), 00110 (code and data), 0 either left (body inline)
        .storeUint(0b10001100, 1 + 4 + 4 + 64 + 32 + (1 + 1 + 5 + 1))
        .storeRef(init.code)
        .storeRef(init.data)
        .storeAddressOpt(body.any1.0)
        .storeBuilder(body.any1.1)
        .storeSlice(body.any2)
        .endCell();
}

@method_id(123)
fun test23(state32: uint32) {
    var init = ContractState {
        data: beginCell().storeUint(state32,32).endCell(),
        code: beginCell().endCell(),
    };
    var bd: Body23Unlimited = {
        any1: (null, beginCell().storeRef(init.code)),
        any2: beginCell().storeUint(80, 80).endCell().beginParse(),
    };

    var b = createMessage({
        bounce: true,
        body: UnsafeBodyNoRef {
            forceInline: bd,
        },
        dest: { workchain: 9, stateInit: init },
        value: ton("0.10009"),
    });
    assert(b.hash() == test23_manual(state32, bd).hash(), 123);
    return b.hash();
}

@noinline
fun test24_manual(addrHash: uint256) {
    return beginCell()
        .storeUint(0x18, 6)            // bounce
        .storeUint(0b100, 3).storeInt(MASTERCHAIN, 8).storeUint(addrHash, 256)
        .storeCoins(ton("0.6"))
        .storeUint(0, 1 + 4 + 4 + 64 + 32 + 1 + 1)  // ... + 0 init + 0 body (not ref)
        .storeUint(0x12345678, 32)
        .storeUint(800, 64)
        .endCell();
}

@method_id(124)
fun test24() {
    val addrHash = address("UQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPuwA").getWorkchainAndHash().1;
    var b = createMessage({
        body: MyBody { queryId: 800 },
        bounce: true,
        dest: (MASTERCHAIN, addrHash),
        value: ton("0.6"),
    });
    assert(b.hash() == test24_manual(addrHash).hash(), 124);
    return b.hash();
}

@noinline
fun test25_manual() {
    return beginCell()
        .storeUint(0x18, 6)            // bounce
        .storeAddress(address("UQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPuwA"))
        .storeCoins(123)   // value.grams
        .storeUint(0, 1)    // no extra currencies
        .storeCoins(3)      // extra_flags for bounces
        .storeUint(0, 4 + 64 + 32 + 1 + 1)  // ... + 0 init + 0 body (not ref)
        // MyBody: op + queryId
        .storeUint(0x12345678, 32)
        .storeUint(800, 64)
        .endCell();
}

@method_id(125)
fun test25() {
    val body: MyBody = { queryId: 800 };
    var b = createMessage({
        body: body,
        bounce: BounceMode.RichBounce,
        dest: address("UQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPuwA"),
        value: 123,
    });
    assert(b.hash() == test25_manual().hash(), 101);
    return b.hash();
}


fun main() {
}

/**
@testcase | 101 |          | 104134380371273907780196393444164225714229635235007677971195651971972203592811
@testcase | 102 |          | 20192939504955637835708665496574868659039935190188156593169026529135727309085
@testcase | 103 |          | 97654287980681401436727082042373998183264988661375688119718809500301413968039
@testcase | 104 | 600000   | 755636693689039391990782995030008885663781699175339033402019272057515711062
@testcase | 105 |          | 24503969966890554363249789782591743702412487637374098978386746563683471749050
@testcase | 106 | 123 null | 104134380371273907780196393444164225714229635235007677971195651971972203592811
@testcase | 106 | 456 8888 | 6425272102977874346517989655071023150283043721608819726435980878339463285784
@testcase | 107 | 1000     | 55093441331748625324828489600632232039914212774002148634088483962817636598198
@testcase | 108 | 50000000 | 95023796475113775225029817428715936488418545169963429399979521091689824066088
@testcase | 109 | 0        | 55999621586681214992294941423256376619779969729861696464321825639854258502733
@testcase | 109 | -1       | 84087871798432599249441213206223143701565541307347047545146076475041341315422
@testcase | 110 | 250 250  | 97468400996544929599099493087921300963923138788231489050737873840992619823773
@testcase | 111 | 1 2 3    | 35151166799433266221446406287469019610025742512503320058804207122452431754243
@testcase | 112 | 32       | 40256061350602595831367445407067573081836468277788226383346273736379122699330
@testcase | 113 |          | 8212062468046185391185852622213582155366804215840270337189205672457136520017
@testcase | 114 | 100500   | 69207815800109735757177433421533576767812185821226447066044060358661780329301
@testcase | 115 | 66 77    | 77952119695754644819736002369288963466111166174255607433142955344110346202253
@testcase | 118 | 888      | 75462675913935779917745192355822465171309245151518256862408373999119088535160
@testcase | 119 | 999      | 20451206881650273118327988889219529836875916996856547550069532964562229905067
@testcase | 120 |          | 86341416901030824925289599533989709413619468614523233983159539599569269903295
@testcase | 121 | 2983742  | 29682465216061902145511914581895871811826194319877849112468723187093436476183
@testcase | 122 | 88 12892 | 104095783372529353117379287284649790470404208891518326338200063605256169961669
@testcase | 123 | 999      | 93691386126134034953606952841435771108648354412078750046821446829279390964584
@testcase | 124 |          | 21886688052816798288463190773103865772534937765373272039597499398551023701577
@testcase | 125 |          | 12745496241890009334837209777890803918215266832624965751220009451486906344811
 */
