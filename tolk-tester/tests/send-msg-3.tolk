/*
ext_out_msg_info$11
    src:MsgAddress              // 00 on send
    dest:MsgAddressExt          // parameter
    created_lt:uint64           // 0 on send
    created_at:uint32           // 0 on send
    = CommonMsgInfoRelaxed;
message$_ {X:Type}
    info:CommonMsgInfoRelaxed
    init:(Maybe (Either StateInit ^StateInit))      // 0 on send
    body:(Either X ^X)                              // body is either embedded or stored as ref
    = MessageRelaxed X;
*/

struct(0x12345678) MyBody {
    queryId: uint64;
}

@noinline
fun test1_manual(topic: int) {
    return beginCell()
        .storeUint(0b1100, 4)       // ext_out_msg_info$11 src:MsgAddressInt ()
        .storeUint(1, 2)            // addr_extern$01
        .storeUint(256, 9)          // len:(## 9)
        .storeUint(topic, 256)      // external_address:(bits len) (assume it fits 248 bits)
        .storeUint(0, 64 + 32 + 2)  // created_lt, created_at, init:Maybe (0), body:Either left (inline)
        // MyBody: op + queryId
        .storeUint(0x12345678, 32)
        .storeUint(800, 64)
        .endCell();
}

@method_id(101)
fun test1(topic: int) {
    val body: MyBody = { queryId: 800 };
    var b = createExternalLogMessage({
        body: body,
        dest: ExtOutLogBucket { topic },
    });
    assert(b.hash() == test1_manual(topic).hash(), 101);
    return b.hash();
}

type Body750Bits = (uint250, uint250, uint250);      // this body is auto-ref

@noinline
fun test2_manual(bd: Body750Bits) {
    val bodyRef = beginCell().storeUint(bd.0,250).storeUint(bd.1,250).storeUint(bd.2,250).endCell();
    return beginCell()
        .storeUint(0b1100, 4)       // ext_out_msg_info$11 src:MsgAddressInt ()
        .storeUint(1, 2)            // addr_extern$01
        .storeUint(256, 9)          // len:(## 9)
        .storeUint(1 << 102, 256)   // external_address:(bits len)
        .storeUint(1, 64 + 32 + 2)  // created_lt, created_at, init:Maybe (0), body:Either right (ref)
        .storeRef(bodyRef)
        .endCell();
}

@method_id(102)
fun test2(bd: Body750Bits) {
    var b = createExternalLogMessage({
        body: bd,
        dest: { topic: 1 << 102 },
    });
    assert(b.hash() == test2_manual(bd).hash(), 102);
    return b.hash();
}

struct Body3 {
    bodyAddress: any_address;
    amount: coins;
    someRef: cell;
}

@method_id(103)
fun test3(eventId: int) {
    val bodyAddress = address("Ef9nzj6RBc4mQ6p3ng7mGJ7tp7MbzERhe7obkM9A0wnCCEcf") as any_address;
    val someRef = MyBody { queryId: 800 }.toCell();
    var b = createExternalLogMessage({
        body: UnsafeBodyNoRef { 
            forceInline: Body3 { bodyAddress, someRef, amount: ton("0.05") } 
        },
        dest: { topic: eventId },
    });
    return b.hash();
}

@noinline
fun test4_manual(dest: any_address, body: cell) {
    return beginCell()
        .storeUint(0b1100, 4)       // ext_out_msg_info$11 src:MsgAddressInt ()
        .storeAddressAny(dest)            // dest (manually created external address)
        .storeUint(1, 64 + 32 + 2)  // created_lt, created_at, init:Maybe (0), body:Either right (ref)
        .storeRef(body)
        .endCell();
}

@method_id(104)
fun test4() {
    val body: cell = Body3 { bodyAddress: createAddressNone(), amount: 0, someRef: createEmptyCell() }.toCell();
    val dest = beginCell().storeUint(0b01, 2).storeUint(12, 9).storeUint(88, 12).endCell()
        .beginParse() as any_address;
    var b = createExternalLogMessage({
        body,
        dest,
    });
    assert(b.hash() == test4_manual(dest, body).hash(), 104);
    return b.hash();
}

@method_id(105)
fun test5(topic: int) {
    var b = createExternalLogMessage({
        dest: { topic },
    });
    return b.hash();
}

@method_id(106)
fun test6() {
    val dest = beginCell().storeUint(0b01, 2).storeUint(12, 9).storeUint(88, 12).endCell()
        .beginParse() as any_address;
    var b = createExternalLogMessage({
        dest,
    });
    return b.hash();
}

@noinline
fun test7_manual(dest: builder, queryId: uint64) {
    return beginCell()
        .storeUint(0b1100, 4)       // ext_out_msg_info$11 src:MsgAddressInt ()
        .storeBuilder(dest)            // manually created bits representing external address
        .storeUint(0, 64 + 32 + 2)  // created_lt, created_at, init:Maybe (0), body:Either left (inline)
        // MyBody: op + queryId
        .storeUint(0x12345678, 32)
        .storeUint(queryId, 64)
        .endCell();
}

@method_id(107)
fun test7(queryId: uint64) {
    val body: MyBody = { queryId };
    val dest = beginCell().storeUint(0b01, 2).storeUint(12, 9).storeUint(88, 12);
    var b = createExternalLogMessage({
        body,
        dest,
    });
    assert(b.hash() == test7_manual(dest, queryId).hash(), 107);
    return b.hash();
}

@method_id(108)
fun test8(topic: int) {
    var bodyB = beginCell().storeUint(0x12345678, 32).storeUint(800, 64);
    var b = createExternalLogMessage({
        body: bodyB,
        dest: { topic },
    });
    return b.hash();
}

struct Body9Unlimited {
    any1: cell?,
    any2: builder,
}

@method_id(109)
fun test9(topic: int) {
    var bd: Body9Unlimited = {
        any1: null,
        any2: beginCell().storeUint(80, 80).storeMaybeRef(null).storeRef(createEmptyCell()),
    };
    var b = createExternalLogMessage({
        body: UnsafeBodyNoRef {
            forceInline: bd,
        },
        dest: ExtOutLogBucket { topic: topic },
    });
    return b.hash();
}

@noinline
fun test10_manual(topic: slice, queryIdRef: uint64) {
    return beginCell()
        .storeUint(0b1100, 4)       // ext_out_msg_info$11 src:MsgAddressInt ()
        .storeUint(1, 2)            // addr_extern$01
        .storeUint(256, 9)          // len:(## 9)
        .storeUint(0x00, 8)         // prefix of ExtOutLogBucket
        .storeSlice(topic)                // assume this slice is bits248
        .storeUint(0, 64 + 32 + 2)  // created_lt, created_at, init:Maybe (0), body:Either left (inline)
        .storeRef(beginCell().storeUint(queryIdRef, 64).endCell())
        .endCell();
}

struct Body10 {
    queryIdRef: Cell<uint64>;
}

@method_id(110)
fun test10() {
    val queryIdRef = 123;
    val topic = stringHexToSlice("012345678901234567890123456789012345678901234567890123456789FF");
    var b = createExternalLogMessage({
        body: Body10 { queryIdRef: (queryIdRef as uint64).toCell() },
        dest: ExtOutLogBucket { topic: topic as bits248 },
    });
    assert(b.hash() == test10_manual(topic, queryIdRef).hash(), 110);
    return b.hash();
}

@method_id(111)
fun test11() {
    try {
        createExternalLogMessage({
            dest: ExtOutLogBucket { topic: "asdf" as bits248 }  // wrong bits count
        });
        return 0;
    } catch (excno) {
        return excno;
    }
}

fun main() {

}

/**
@testcase | 101 | 100500 | 8399954633429803564068357060778961153860921657197909539205508789211284886940
@testcase | 102 | 1 2 3  | 64371641017422166665232911624005896033238822098768040861805756890339950064655
@testcase | 103 | 829999 | 33039908261421267188072304726227928987040300641614103065684678216731595546507
@testcase | 104 |        | 94681566274361007141065727833953679134419126606639224444485202401996571399154
@testcase | 105 | 329889 | 24202801754361933063453004029355483858376498707977868951519201997233775688005
@testcase | 106 |        | 17838299862930869444234264325786950440464929864630237676886309244018701054708
@testcase | 107 | 777    | 77493748753370371314291958955179535305777482888906702570602071501642726442389
@testcase | 108 | 0      | 69287572703823759594480986943855745035158339431740068559880301501812484359455
@testcase | 109 | 12     | 72694183679327534355706107347199645597776533707531061585024123910830113048519
@testcase | 110 |        | 33186381366677843958628736306577552541950719844529663083708985644217867888324
@testcase | 111 |        | 9
 */
