const SHARD_DEPTH = 8;

fun getMyAddressDev(): address
    asm "x{80194DC6438F99D3D9DBE151944925D90B2492954BF6B9C070FBFF2DDED5F30547D_} PUSHSLICE";

struct WalletStorage {
    balance: coins;
    ownerAddress: address;
    minterAddress: address;
}

fun WalletStorage.generateEmptyData(ownerAddress: address, minterAddress: address) {
    val emptyWalletStorage: WalletStorage = {
        balance: 0,
        ownerAddress,
        minterAddress,
    };
    return emptyWalletStorage.toCell();
}

@inline_ref
fun buildAddrInShard_manual(a: address, options: AddressShardingOptions) {
    var sb = options.closeTo as slice;
    sb.skipBits(3);         // addr_std$10 + anycast 0
    val wc_b = sb.loadInt(8);
    val shardPrefix = sb.loadUint(options.fixedPrefixLength);

    var sa = a as slice;
    sa.skipBits(3 + 8 + options.fixedPrefixLength);

    return beginCell()
        .storeUint(0b100, 3)        // addr_std$10 + anycast 0
        .storeInt(wc_b, 8)
        .storeUint(shardPrefix, options.fixedPrefixLength)
        .storeSlice(sa);
}

@method_id(101)
fun test1() {
    val a = address("0:00000000000000000000000000000000000000000000000000000000000000FF");
    val b = address("1:1100000000000000000000000000000000000000000000000000000000000000");
    val dd1 = buildAddrInShard_manual(a, {fixedPrefixLength: 8, closeTo: b});
    val dd2 = a.buildSameAddressInAnotherShard({fixedPrefixLength: 8, closeTo: b});
    assert(dd1.endCell().hash() == dd2.endCell().hash(), 400);
    return dd1.endCell().beginParse() as address
    == address("1:11000000000000000000000000000000000000000000000000000000000000FF");
}

@method_id(102)
fun test2() {
    val a = address("0:1234aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    val b = address("0:FFFFbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    val dd1 = buildAddrInShard_manual(a, {closeTo: b, fixedPrefixLength: 16});
    val dd2 = a.calculateSameAddressInAnotherShard({fixedPrefixLength: 16, closeTo: b});
    assert((a as slice).remainingBitsCount() == 267, 267);
    assert((b as slice).remainingBitsCount() == 267, 267);
    assert(dd1.endCell().hash() == (dd2 as slice).hash(), 400);
    return dd2 == address("0:FFFFaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
}

@method_id(103)
fun test3() {
    var t1_a = address("EQCRDM9h4k3UJdOePPuyX40mCgA4vxge5Dc5vjBR8djbEKC5");
    var t1_b = address("9:527964d55cfa6eb731f4bfc07e9d025098097ef8505519e853986279bd8400d8");
    var t2_a = address("EQCe4AYIBce1pAk2qJJPSs1OzyZRlKjkfq8zuC8D7erv6DUP");
    var t2_b = address("EQCtrtTXEAoSpoERmiqOnICe9LHxn2N89N4BH9qdHlrG-U0i");
    var t3_a = address("EQAUTbQiM522Y_XJ_T98QPhPhTmb4nV--VSPiha8kC6kRfPO");
    var t3_b = address("EQBlqsm144Dq6SjbPI4jjZvA1hqTIP3CvHovbIfW_t-SCALE");
    return (
        (t1_a.calculateSameAddressInAnotherShard({fixedPrefixLength: 8, closeTo: t1_b}) as slice).hash() & 0xFFFF,
        (t1_a.calculateSameAddressInAnotherShard({fixedPrefixLength: 4, closeTo: t1_b}) as slice).hash() & 0xFFFF,
        (t2_a.calculateSameAddressInAnotherShard({fixedPrefixLength: 2, closeTo: t2_b}) as slice).hash() & 0xFFFF,
        (t3_a.calculateSameAddressInAnotherShard({fixedPrefixLength: 30, closeTo: t2_b}) as slice).hash() & 0xFFFF,
        (t2_a.calculateSameAddressInAnotherShard({fixedPrefixLength: 1, closeTo: t3_b}) as slice).hash() & 0xFFFF,
        (t2_a.calculateSameAddressInAnotherShard({fixedPrefixLength: 0, closeTo: t3_b}) as slice).hash() & 0xFFFF,
    )
}

@method_id(104)
fun test4(shardDepth: int) {
    var a = address("EQCRDM9h4k3UJdOePPuyX40mCgA4vxge5Dc5vjBR8djbEKC5");
    var b = address("9:527964d55cfa6eb731f4bfc07e9d025098097ef8505519e853986279bd8400d8");
    return a.buildSameAddressInAnotherShard({fixedPrefixLength: shardDepth, closeTo: b}).endCell().hash() & 0xFFFF;
}

@method_id(105)
fun test5() {
    var b = beginCell()
        .storeUint(0b100, 3)       // std addr no anycast
        .storeInt(MASTERCHAIN, 8)
        .storeUint(0xFFFF, 256);
    val a = address("-1:000000000000000000000000000000000000000000000000000000000000FFFF");
    return address.fromValidBuilder(b) == a;
}

@method_id(106)
fun test6() {
    val a = address("0:00000000000000000000000000000000000000000000000000000000000000FF");
    val b = address("1:1100000000000000000000000000000000000000000000000000000000000000");
    return  a.calculateSameAddressInAnotherShard({fixedPrefixLength: 8, closeTo: b}) ==
    address("1:11000000000000000000000000000000000000000000000000000000000000FF");
}


fun manual_buildAddressOfJettonWallet_plain(ownerAddress: address, minterAddress: address, jettonWalletCode: cell): builder {
    val stateInitHash = StateInit.calcHashCodeData(
        jettonWalletCode,
        WalletStorage.generateEmptyData(ownerAddress, minterAddress)
    );
    return beginCell()
        .storeUint(0b100, 3)
        .storeUint(BASECHAIN, 8)
        .storeUint(stateInitHash, 256)
}

fun manual_buildAddressOfJettonWallet_sharded(ownerAddress: address, minterAddress: address, jettonWalletCode: cell): builder {
    val stateInitHash = StateInit.calcHashPrefixCodeData(
        SHARD_DEPTH,
        jettonWalletCode,
        WalletStorage.generateEmptyData(ownerAddress, minterAddress)
    );
    var mask = stateInitHash & ((1 << (256 - SHARD_DEPTH)) - 1);
    val shard_prefix = (ownerAddress as slice).getMiddleBits(3 + 8, SHARD_DEPTH);
    return beginCell()
        .storeUint(0b100, 3)
        .storeUint(BASECHAIN, 8)
        .storeSlice(shard_prefix)
        .storeUint( mask, 256 - SHARD_DEPTH);
}

fun address.manual_isAddressOfJettonWallet_plain(self, ownerAddress: address, minterAddress: address, jettonWalletCode: cell) {
    val stateInitHash = StateInit.calcHashCodeData(
        jettonWalletCode,
        WalletStorage.generateEmptyData(ownerAddress, minterAddress)
    );
    val (wc, hash) = self.getWorkchainAndHash();
    return (stateInitHash == hash) & (BASECHAIN == wc);
}

fun address.manual_isAddressOfJettonWallet_sharded(self, ownerAddress: address, minterAddress: address, jettonWalletCode: cell) {
    var stateInitHash = StateInit.calcHashPrefixCodeData(
        SHARD_DEPTH,
        jettonWalletCode,
        WalletStorage.generateEmptyData(ownerAddress, minterAddress)
    );
    val mask = (1 << (256 - SHARD_DEPTH)) - 1;
    stateInitHash = stateInitHash & mask;
    var (wc, hash) = self.getWorkchainAndHash();
    hash = hash & mask;
    return (stateInitHash == hash) & (BASECHAIN == wc);
}

@method_id(110)
fun test10() {
    val jwCode: cell = beginCell().storeInt(0x273849723892, 94).endCell();
    val ownerAddress = address("EQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPrHF");
    val minterAddress = getMyAddressDev();

    val b1 = manual_buildAddressOfJettonWallet_plain(ownerAddress, minterAddress, jwCode);
    val si2: AutoDeployAddress = {
        stateInit: { code: jwCode, data: WalletStorage.generateEmptyData(ownerAddress, minterAddress) }
    };
    val b2 = si2.buildAddress();
    assert (b1.endCell().hash() == b2.endCell().hash()) throw 123;

    val checkedAddr = b1.endCell().beginParse() as address;
    assert (checkedAddr.manual_isAddressOfJettonWallet_plain(ownerAddress, minterAddress, jwCode)) throw 123;
    assert (si2.addressMatches(checkedAddr)) throw 123;

    return (
        si2.addressMatches(ownerAddress),
        AutoDeployAddress {
            workchain: -1,
            stateInit: { code: jwCode, data: WalletStorage.generateEmptyData(ownerAddress, minterAddress) }
        }.addressMatches(checkedAddr)
    )
}

@method_id(111)
fun test11() {
    val jwCode: cell = beginCell().storeInt(0x273849723892, 94).endCell();
    val ownerAddress = address("EQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPrHF");
    val minterAddress = getMyAddressDev();

    val b1 = manual_buildAddressOfJettonWallet_sharded(ownerAddress, minterAddress, jwCode);
    val si2: AutoDeployAddress = {
        stateInit: { code: jwCode, data: WalletStorage.generateEmptyData(ownerAddress, minterAddress) },
        toShard: { fixedPrefixLength: SHARD_DEPTH, closeTo: ownerAddress }
    };
    val b2 = si2.buildAddress();
    assert (b1.endCell().hash() == b2.endCell().hash()) throw 123;

    val checkedAddr = b1.endCell().beginParse() as address;
    assert (checkedAddr.manual_isAddressOfJettonWallet_sharded(ownerAddress, minterAddress, jwCode)) throw 123;
    assert (si2.addressMatches(checkedAddr)) throw 123;

    return (
        AutoDeployAddress {
            stateInit: { code: jwCode, data: WalletStorage.generateEmptyData(ownerAddress, minterAddress) },
        }.addressMatches(checkedAddr),
        si2.addressMatches(ownerAddress),
        AutoDeployAddress {
            workchain: -1,
            stateInit: { code: jwCode, data: WalletStorage.generateEmptyData(ownerAddress, minterAddress) },
            toShard: { fixedPrefixLength: SHARD_DEPTH, closeTo: ownerAddress }
        }.addressMatches(checkedAddr)
    )
}

@method_id(112)
fun test12() {
    val jwCode: cell = beginCell().storeInt(0x273849723892, 94).endCell();
    val ownerAddress = address("EQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPrHF");
    var i = 1;
    while (i < 10) {
        var params: AutoDeployAddress = {
            workchain: -1,
            stateInit: { code: jwCode, data: createEmptyCell() },
            toShard: { fixedPrefixLength: i, closeTo: ownerAddress }
        };
        val addr = params.calculateAddress();
        assert (params.addressMatches(addr)) throw 123;
        params.workchain = 0;
        assert (!params.addressMatches(addr)) throw 123;
        params.workchain = -1;
        params.toShard!.fixedPrefixLength += 1;
        assert (!params.addressMatches(addr)) throw 123;
        i += 1
    }
    return i;
}

@method_id(113)
fun test13() {
    val stateInitCell = StateInit {
        fixedPrefixLength: null,
        code: createEmptyCell(),
        data: beginCell().storeUint(123, 32).endCell(),
        special: null,
        library: null,
    }.toCell();

    val manualBuilt = beginCell().storeUint(0b100, 3).storeUint(0, 8)
                             .storeUint(stateInitCell.hash(), 256);
    val si: AutoDeployAddress = {
        stateInit: stateInitCell
    };
    val addrBuilt = si.buildAddress();
    assert (manualBuilt.endCell().hash() == addrBuilt.endCell().hash()) throw 123;

    val addr = addrBuilt.endCell().beginParse() as address;
    return (
        si.addressMatches(addr),
        AutoDeployAddress {workchain: 99, stateInit: stateInitCell}.addressMatches(addr),
    )
}

@method_id(114)
fun test14() {
    val ownerAddress = address("EQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPrHF");
    val stateInitCell = StateInit {
        fixedPrefixLength: 10,
        code: createEmptyCell(),
        data: beginCell().storeUint(123, 32).endCell(),
        special: null,
        library: null,
    }.toCell();

    val manualBuilt = beginCell().storeUint(0b100, 3).storeUint(9, 8)
                             .storeSlice((ownerAddress as slice).getMiddleBits(3+8, 10))
                             .storeUint(stateInitCell.hash() & ((1 << 246) - 1), 246);
    val si: AutoDeployAddress = {
        workchain: 9,
        stateInit: stateInitCell,
        toShard: { fixedPrefixLength: 10, closeTo: ownerAddress }
    };
    val addr = si.calculateAddress();

    val stateInitHash = stateInitCell.hash();
    val (wc, hash) = addr.getWorkchainAndHash();
    val manual_isAddrOfContract = ((stateInitHash & ((1 << 246) - 1)) == (hash & ((1 << 246) - 1))) & (9 == wc);

    return (
        manualBuilt.endCell().hash() == (addr as slice).hash(),
        manual_isAddrOfContract,
        si.addressMatches(addr),
    )
}

@method_id(115)
fun test15() {
    var any = createAddressNone();
    var addr = address("EQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPrHF");

    any as any_address;
    any as address;
    (any as slice) as any_address;
    addr as any_address;
    addr as address;
    (addr as slice) as address;

    return (
        any == addr, any != addr,
        (any as address) == addr, any == (addr as any_address),
        (any as slice).bitsEqual(addr as slice),
        (any as bits123 as any_address as slice).remainingBitsCount(),
        (addr as bytes1 as address as slice).remainingBitsCount(),
    )
}

@method_id(116)
fun test16() {
    val any = address("Ef9nzj6RBc4mQ6p3ng7mGJ7tp7MbzERhe7obkM9A0wnCCEcf") as any_address;
    val addr = any.castToInternal();
    return (
        addr == any, (any as slice).remainingBitsCount(), (addr as slice).remainingBitsCount(),
        addr.getWorkchain() == any.getWorkchain(),
        addr.getWorkchainAndHash().1 == any.getWorkchainAndHash().1,
    );
}

@method_id(117)
fun test17() {
    val ex = beginCell().storeUint(0b01, 2).storeUint(6, 9).storeUint(0, 6).endCell().beginParse() as any_address;
    val in = address("Ef9nzj6RBc4mQ6p3ng7mGJ7tp7MbzERhe7obkM9A0wnCCEcf") as any_address;
    val no = createAddressNone();
    __expect_type(no, "any_address");

    assert (ex.isExternal()) throw 123;
    assert (in.isInternal()) throw 123;
    assert (no.isNone()) throw 123;
    return (ex.isInternal(), ex.isNone(), in.isExternal(), in.isNone(), no.isInternal(), no.isExternal());
}

@method_id(118)
fun test18(): (int, int8) {
    try {
        val any = createAddressNone();
        return (0, any.castToInternal().getWorkchain());
    } catch (ex) {
        try {
            return (ex, createAddressNone().getWorkchain())
        } catch (ex2) {
            return (ex, ex2);
        }
    }
}

@method_id(119)
fun test19() {
    val a = address("EQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPrHF");

    val c1 = a.toCell();
    val c2 = (a as address?).toCell();
    val c3 = (a as any_address).toCell();
    assert (c1.hash() == c2.hash()) throw 123;
    assert (c1.hash() == c3.hash()) throw 123;

    val e1 = (null as address?).toCell();
    val e2 = createAddressNone().toCell();
    assert (e1.hash() == e2.hash()) throw 123;

    return e1.beginParse().remainingBitsCount();
}


fun main() {}

/**
@testcase | 101 |   | -1
@testcase | 102 |   | -1
@testcase | 103 |   | 39876 24338 15241 50719 11252 15241
@testcase | 104 | 2 | 24338
@testcase | 104 | 9 | 39876
@testcase | 105 |   | -1
@testcase | 106 |   | -1
@testcase | 110 |   | 0 0
@testcase | 111 |   | 0 0 0
@testcase | 112 |   | 10
@testcase | 113 |   | -1 0
@testcase | 114 |   | -1 -1 -1
@testcase | 115 |   | 0 -1 0 0 0 2 267
@testcase | 116 |   | -1 267 267 -1 -1
@testcase | 117 |   | 0 0 0 0 0 0
@testcase | 118 |   | 9 9
@testcase | 119 |   | 2
 */
