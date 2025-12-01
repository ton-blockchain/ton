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

const SHARD_DEPTH = 8;

fun getMyAddressDev(): address
    asm "x{80194DC6438F99D3D9DBE151944925D90B2492954BF6B9C070FBFF2DDED5F30547D_} PUSHSLICE";

@inline
fun getAddressShard(address: address, shardLen: int): int {
    // Skip workchain, load shard prefix
    (address as slice).skipBits(3 + 8);
    return (address as slice).loadUint(shardLen);
}

@inline
fun packJettonWalletData(status: int, balance: int, ownerAddress: address, jettonMasterAddress: address): cell {
    return beginCell()
        .storeUint(status, 8)
        .storeCoins(balance)
        .storeAddress(ownerAddress)
        .storeAddress(jettonMasterAddress)
        .endCell();
}

@inline
fun calculateJettonWalletStateInitWithShard(ownerAddress: address, jettonMasterAddress: address, jettonWalletCode: cell): cell {
    /*
    https://github.com/ton-blockchain/ton/blob/8a9ff339927b22b72819c5125428b70c406da631/crypto/block/block.tlb#L144
    _ split_depth:(Maybe (## 5)) special:(Maybe TickTock)
    code:(Maybe ^Cell) data:(Maybe ^Cell)
    library:(Maybe ^Cell) = StateInit;
  */
    return beginCell()
        .storeUint(1, 1)
        .storeUint(SHARD_DEPTH, 5)
        .storeUint(0, 1)
        .storeMaybeRef(jettonWalletCode)
        .storeMaybeRef(
        packJettonWalletData(
            0, // status
            0, // balance
            ownerAddress,
            jettonMasterAddress)
    )
        .storeUint(0, 1) // Empty libraries
        .endCell();
}

@inline
fun calculateJettonWalletAddress(shardPrefix: int, stateInitCell: cell): address {
    var mask = (1 << (256 - SHARD_DEPTH)) - 1;
    var prefixLess = stateInitCell.hash() & mask;
    return beginCell()
        .storeUint(4, 3) // addr_std$10 + anycast 0
        .storeInt(BASECHAIN, 8)
        .storeUint(shardPrefix, SHARD_DEPTH)
        .storeUint(prefixLess, 256 - SHARD_DEPTH)
        .endCell()
        .beginParse() as address;
}

@inline
fun calculateAddressInAnotherShard(pivotAddress: address, shardPrefixLen: uint5, code: cell, data: cell): builder {
    val stateInitCell = beginCell()
        .storeUint(1, 1)
        .storeUint(shardPrefixLen, 5)   // shard depth
        .storeUint(0, 1)
        .storeMaybeRef(code)
        .storeMaybeRef(data)
        .storeUint(0, 1) // Empty libraries
        .endCell();

    val shardPrefix = getAddressShard(pivotAddress, shardPrefixLen);
    var mask = (1 << (256 - shardPrefixLen)) - 1;
    var prefixLess = stateInitCell.hash() & mask;
    return beginCell()
        .storeUint(4, 3) // addr_std$10 + anycast 0
        .storeInt(BASECHAIN, 8)
        .storeUint(shardPrefix, shardPrefixLen)
        .storeUint(prefixLess, 256 - shardPrefixLen);
}

@noinline
fun test1_manual(toAddress: address, masterMsg: cell, jettonWalletCode: cell) {
    var shardPrefix = getAddressShard(toAddress, SHARD_DEPTH);
    var stateInitCell = calculateJettonWalletStateInitWithShard(toAddress, getMyAddressDev(), jettonWalletCode);
    var toWalletAddress = calculateJettonWalletAddress(shardPrefix, stateInitCell);
    var jettonWalletData =  packJettonWalletData(0, 0, toAddress, getMyAddressDev());

    return beginCell()
        .storeUint(0x18, 6)     // bounceable
        .storeAddress(toWalletAddress) // dest
        .storeCoins(ton("0.05"))
        // 1 state init exists, 0 either left (state init embedded), 1 either left (fixed_prefix_length exists)
        .storeUint(0b101, 1 + 4 + 4 + 64 + 32 + 1 + 1 + 1)
        .storeUint(SHARD_DEPTH, 5)
        .storeUint(0b01101, 4 + 1)  // 0 ticktock + 1 code + 1 data + 0 library + 1 body ref
        .storeRef(jettonWalletCode)
        .storeRef(jettonWalletData)
        .storeRef(masterMsg)
        .endCell();
}

@method_id(101)
fun test1() {
    val toAddress = address("UQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPuwA");
    val masterMsg = beginCell().storeUint(1<<14, 32).endCell();
    val jettonWalletCode = beginCell().storeInt(0x273849723892, 94).endCell();

    var b = createMessage({
        bounce: BounceMode.Only256BitsOfBody,
        dest: {
            stateInit: {
                code: jettonWalletCode,
                data: packJettonWalletData(0, 0, toAddress, getMyAddressDev()),
            },
            toShard: {
                fixedPrefixLength: SHARD_DEPTH,
                closeTo: toAddress,
            }
        },
        body: masterMsg,
        value: ton("0.05"),
    });
    assert(b.hash() == test1_manual(toAddress, masterMsg, jettonWalletCode).hash(), 101);
    return b.hash()
}

@noinline
fun test2_manual(toAddress: address, masterMsg: cell, jettonWalletCode: cell) {
    var shardPrefix = getAddressShard(toAddress, SHARD_DEPTH);
    var stateInit = calculateJettonWalletStateInitWithShard(toAddress, getMyAddressDev(), jettonWalletCode);
    var toWalletAddress = calculateJettonWalletAddress(shardPrefix, stateInit);

    return beginCell()
        .storeUint(0x18, 6)     // bounceable
        .storeAddress(toWalletAddress) // dest
        .storeCoins(ton("0.05"))
        .storeUint(0, 1)    // no extra currencies
        .storeCoins(1)          // extra_flags for bounces
        .storeUint(0b111, 4 + 64 + 32 + 1 + 1 + 1)  // 1 state init exists + 1 state init ref + 1 body ref
        .storeRef(stateInit)
        .storeRef(masterMsg)
        .endCell();
}

@method_id(102)
fun test2() {
    val toAddress = address("UQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPuwA");
    val masterMsg = beginCell().endCell();
    val jettonWalletCode = beginCell().storeInt(0x273849723892, 94).endCell();
    var stateInitCell = calculateJettonWalletStateInitWithShard(toAddress, getMyAddressDev(), jettonWalletCode);

    var b = createMessage({
        bounce: BounceMode.RichBounceOnlyRootCell,
        dest: {
            workchain: BASECHAIN,
            stateInit: stateInitCell,
            toShard: {
                fixedPrefixLength: SHARD_DEPTH,
                closeTo: toAddress,
            }
        },
        body: masterMsg,
        value: ton("0.05"),
    });
    assert(b.hash() == test2_manual(toAddress, masterMsg, jettonWalletCode).hash(), 102);
    return b.hash()
}

struct Test3Body {
    bigData: bits800;
}

@noinline
fun test3_manual(myCode: cell, myData: cell, msgBody: Test3Body) {
    var addrInShard = calculateAddressInAnotherShard(getMyAddressDev(), 20, myCode, myData);

    return beginCell()
        .storeUint(0x18, 6)     // bounceable
        .storeBuilder(addrInShard) // dest
        .storeCoins(ton("0.001"))
        .storeUint(0, 1)    // no extra currencies
        .storeCoins(3)          // extra_flags for bounces
        .storeUint(0b10, 4 + 64 + 32 + 1 + 1)  // 1 state init exists + 1 state init inline
            .storeUint(1, 1)
            .storeUint(20, 5)   // shard depth
            .storeUint(0b0110, 4) // code + data exist
        .storeUint(1, 1) // body ref
        .storeRef(myCode)
        .storeRef(myData)
        .storeRef(msgBody.toCell())
        .endCell();
}

@method_id(103)
fun test3() {
    val myCode = beginCell().storeInt(0x273849723892, 94).endCell();
    val myData = beginCell().storeInt(0x273849723892, 200).storeMaybeRef(myCode).endCell();
    val builder800 = beginCell().storeInt(1, 250).storeInt(1, 250).storeInt(1, 250).storeInt(1, 50);
    val body: Test3Body = { bigData: builder800.endCell().beginParse() as bits800 };

    var b = createMessage({
        bounce: BounceMode.RichBounce,
        dest: {
            stateInit: {
                code: myCode,
                data: myData,
            },
            toShard: {
                fixedPrefixLength: 20,
                closeTo: getMyAddressDev(),
            }
        },
        body,
        value: ton("0.001"),
    });
    assert(b.hash() == test3_manual(myCode, myData, body).hash(), 103);
    return b.hash()
}

struct Test4Body {
    f1: int64;
    f2: int64;
}

@method_id(104)
fun test4() {
    val myCode = beginCell().storeInt(0x273849723892, 94).endCell();
    val myData = beginCell().storeInt(0x273849723892, 200).endCell();
    val body: Test4Body = { f1: 123, f2: 456 };

    var b = createMessage({
        bounce: true,
        dest: {
            stateInit: {
                code: myCode,
                data: myData,
            },
            toShard: {
                fixedPrefixLength: 30,
                closeTo: getMyAddressDev(),
            }
        },
        body,
        value: ton("0.1"),
    });
    return b.hash()
}

@overflow1023_policy("suppress")
struct Test5Body {
    c1: coins; c2: coins; c3: coins;
    c4: coins; c5: coins; c6: coins;
    c7: coins; c8: coins; c9: coins;
}

@method_id(105)
fun test5() {
    val pivot = address("1:527964d55cfa6eb731f4bfc07e9d025098097ef8505519e853986279bd8400d8");
    val myCode = beginCell().storeInt(0x273849723892, 94).endCell();
    val myData = beginCell().storeInt(0x273849723892, 200).endCell();
    val body: Test5Body = { c1: 1, c2: 12, c3: 13, c4: 41, c5: 15, c6: 66, c7: 777, c8: 8888, c9: 0x9999 };

    var b = createMessage({
        bounce: true,
        dest: {
            stateInit: {
                code: myCode,
                data: myData,
            },
            toShard: {
                fixedPrefixLength: 2,
                closeTo: pivot,
            }
        },
        body: UnsafeBodyNoRef { forceInline: body },
        value: ton("0.1"),
    });
    return b.hash()
}

@method_id(106)
fun test6(shardLen: int) {
    val pivot = address("1:527964d55cfa6eb731f4bfc07e9d025098097ef8505519e853986279bd8400d8");
    val myCode = beginCell().storeInt(0x273849723892, 94).endCell();
    val myData = beginCell().storeInt(0x273849723892, 200).endCell();
    val body: Test5Body = { c1: 1, c2: 12, c3: 13, c4: 41, c5: 15, c6: 66, c7: 777, c8: 8888, c9: 0x9999 };

    var b = createMessage({
        bounce: true,
        dest: {
            workchain: BASECHAIN,
            stateInit: {
                code: myCode,
                data: myData,
            },
            toShard: {
                fixedPrefixLength: shardLen,
                closeTo: pivot,
            }
        },
        body: body,
        value: ton("0.1"),
    });
    return b.hash()
}

fun main() {}


/**
@testcase | 101 |   | 114692697425779687591180352274328586758984823673144676907237399062035787598876
@testcase | 102 |   | 92833870282045340164651591875742409601508873505925232125500574773472682371231
@testcase | 103 |   | 56762665750429421964960025520433779418560428561961050303426782106579738725679
@testcase | 104 |   | 52060388210984960140400107426752119783720473036191582802152450095687953630545
@testcase | 105 |   | 18248582963085351579261363895401647616194364201057112283341027605548475235397
@testcase | 106 | 2 | 73367500160949078368695742399322272058735198813159066885436981754975296689213
@testcase | 106 | 9 | 26260034723231226694451786564335585031223880314377072827334911071856417531179
 */
