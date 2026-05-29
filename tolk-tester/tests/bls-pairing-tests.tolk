
fun bls_g1_add(a: slice, b: slice): slice
    asm "BLS_G1_ADD"
fun bls_g1_sub(a: slice, b: slice): slice
    asm "BLS_G1_SUB"
fun bls_g1_neg(a: slice): slice
    asm "BLS_G1_NEG"
fun bls_g1_mul(p: slice, x: int): slice
    asm "BLS_G1_MUL"
fun bls_g1_in_group(p: slice): bool
    asm "BLS_G1_INGROUP"
fun bls_g1_is_zero(p: slice): bool
    asm "BLS_G1_ISZERO"
fun bls_g2_in_group(p: slice): bool
    asm "BLS_G2_INGROUP"
fun bls_map_to_g1(fp: slice): slice
    asm "BLS_MAP_TO_G1"
fun bls_map_to_g2(fp2: slice): slice
    asm "BLS_MAP_TO_G2"

fun bls_verify(pub: slice, msg: slice, sig: slice): bool
    asm "BLS_VERIFY"
fun bls_aggregate_1(sig: slice, n: int): slice
    asm "BLS_AGGREGATE"
fun bls_fast_aggregate_verify_1(pub: slice, n: int, msg: slice, sig: slice): bool
    asm "BLS_FASTAGGREGATEVERIFY"
fun bls_aggregate_verify_1(pub: slice, msg: slice, n: int, sig: slice): bool
    asm "BLS_AGGREGATEVERIFY"

fun bls_g1_multiexp_2(p1: slice, k1: int, p2: slice, k2: int): slice
    asm "2 PUSHINT BLS_G1_MULTIEXP"
fun bls_pairing_1(p1: slice, p2: slice): bool
    asm "1 PUSHINT BLS_PAIRING"
fun bls_pairing_2(p1: slice, p2: slice, p3: slice, p4: slice): bool
    asm "2 PUSHINT BLS_PAIRING"

// G1 cofactor-domain point (on-curve, but NOT in-group G1)
const INVALID_G1_HEX = "800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000004"

// G2 cofactor-domain point (on-curve, but NOT in-group G2)
const INVALID_G2_HEX = "800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002"

const VALID_FP_HEX_A = "7abd13983c76661a98659da83066c71bd6581baf20c82c825b007bf8057a258dc53f7a6d44fb6fdecb63d9586e845d92";
const VALID_FP_HEX_B = "1f0a4d68ce25c0a91296ee2c1c2e8db84cd3220e88c0c3a214bb52c33d27e3a6f24aef0e6f4a8d56cf5e8c64a0c91dde";
const VALID_FP2_HEX = "cce34c6322b8f3b455617a975aff8b6eaedf04fbae74a8890db6bc3fab0475b94cd8fbde0e1182ce6993afd56ed6e71919cae59c891923b4014ed9e42d9f0e1a779d9a7edb64f5e2fd600012805fc773b5092af5d2f0c6c0946ee9ad8394bf19";
const VALID_PUB_HEX = "97f1d3a73197d7942695638c4fa9ac0fc3688c4f9774b905a14e3a3f171bac586c55e83ff97a1aeffb3af00adb22c6bb";
const VALID_SIG_HEX = "a42ae16f1c2a5fa69c04cb5998d2add790764ce8dd45bf25b29b4700829232052b52352dcff1cf255b3a7810ad7269601810f03b2bc8b68cf289cf295b206770605a190b6842583e47c3d1c0f73c54907bfb2a602157d46a4353a20283018763";
const VALID_MSG_HEX = "1212121212121212121212121212121212121212121212121212121212121212";

fun valid_g1_A()  { return bls_map_to_g1(VALID_FP_HEX_A.hexToSlice()) }
fun valid_g1_B()  { return bls_map_to_g1(VALID_FP_HEX_B.hexToSlice()) }
fun valid_g2()    { return bls_map_to_g2(VALID_FP2_HEX.hexToSlice()) }
fun valid_pub()   { return VALID_PUB_HEX.hexToSlice() }
fun valid_sig()   { return VALID_SIG_HEX.hexToSlice() }
fun invalid_g1()  { return INVALID_G1_HEX.hexToSlice() }
fun invalid_g2()  { return INVALID_G2_HEX.hexToSlice() }
fun valid_msg()   { return VALID_MSG_HEX.hexToSlice() }


@method_id(100)
fun b_add_invalid_first(): int {
    try {
        bls_g1_add(invalid_g1(), valid_g1_A());
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(101)
fun b_sub_invalid_second(): int {
    try {
        bls_g1_sub(valid_g1_A(), invalid_g1());
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(102)
fun b_mul_invalid(): int {
    try {
        bls_g1_mul(invalid_g1(), 42);
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(103)
fun b_neg_invalid(): int {
    try {
        bls_g1_neg(invalid_g1());
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(104)
fun b_multiexp_invalid(): int {
    try {
        bls_g1_multiexp_2(invalid_g1(), 7, valid_g1_A(), 3);
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(105)
fun b_pairing_invalid_g1(): int {
    try {
        return bls_pairing_1(invalid_g1(), valid_g2()) ? -1 : 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(106)
fun b_pairing_invalid_g2(): int {
    try {
        return bls_pairing_1(valid_g1_A(), invalid_g2()) ? -1 : 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(107)
fun b_add_invalid_second(): int {
    try {
        bls_g1_add(valid_g1_A(), invalid_g1());
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(108)
fun b_sub_invalid_first(): int {
    try {
        bls_g1_sub(invalid_g1(), valid_g1_A());
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(200)
fun test200(): bool {
    return bls_g1_in_group(bls_g1_add(valid_g1_A(), valid_g1_B()));
}

@method_id(201)
fun test201(): bool {
    var p = valid_g1_A();
    return bls_g1_is_zero(bls_g1_add(p, bls_g1_neg(p)));
}

@method_id(202)
fun test202(): bool {
    var p = valid_g1_A();
    return bls_g1_is_zero(bls_g1_sub(p, p));
}

@method_id(203)
fun test203(): bool {
    var p = valid_g1_A();
    var via_mul = bls_g1_mul(p, 3);
    var via_add = bls_g1_add(bls_g1_add(p, p), p);
    return bls_g1_is_zero(bls_g1_sub(via_mul, via_add));
}

@method_id(204)
fun test204(): bool {
    var p1 = valid_g1_A();
    var p2 = valid_g1_B();
    var via_multiexp = bls_g1_multiexp_2(p1, 5, p2, 7);
    var via_manual   = bls_g1_add(bls_g1_mul(p1, 5), bls_g1_mul(p2, 7));
    return bls_g1_is_zero(bls_g1_sub(via_multiexp, via_manual));
}

@method_id(205)
fun test205(): bool {
    var p = valid_g1_A();
    var q = valid_g2();
    return bls_pairing_2(p, q, bls_g1_neg(p), q);
}

@method_id(206)
fun test206(): bool {
    return bls_pairing_1(valid_g1_A(), valid_g2());
}

@method_id(300)
fun h_verify_valid() {
    return bls_verify(valid_pub(), valid_msg(), valid_sig());
}

@method_id(301)
fun h_verify_invalid_pub() {
    return bls_verify(invalid_g1(), valid_msg(), valid_sig());
}

@method_id(302)
fun h_verify_invalid_sig() {
    return bls_verify(valid_pub(), valid_msg(), invalid_g2());
}

@method_id(303)
fun h_fast_aggregate_verify_valid() {
    return bls_fast_aggregate_verify_1(valid_pub(), 1, valid_msg(), valid_sig());
}

@method_id(304)
fun h_fast_aggregate_verify_invalid_pub() {
    return bls_fast_aggregate_verify_1(invalid_g1(), 1, valid_msg(), valid_sig());
}

@method_id(305)
fun h_fast_aggregate_verify_invalid_sig() {
    return bls_fast_aggregate_verify_1(valid_pub(), 1, valid_msg(), invalid_g2());
}

@method_id(306)
fun h_aggregate_verify_valid() {
    return bls_aggregate_verify_1(valid_pub(), valid_msg(), 1, valid_sig());
}

@method_id(307)
fun h_aggregate_verify_invalid_pub() {
    return bls_aggregate_verify_1(invalid_g1(), valid_msg(), 1, valid_sig());
}

@method_id(308)
fun h_aggregate_verify_invalid_sig() {
    return bls_aggregate_verify_1(valid_pub(), valid_msg(), 1, invalid_g2());
}

@method_id(309)
fun h_aggregate_single_valid_sig_is_in_group() {
    return bls_g2_in_group(bls_aggregate_1(valid_sig(), 1));
}

@method_id(310)
fun h_aggregate_single_invalid_sig_is_not_checked() {
    return bls_g2_in_group(bls_aggregate_1(invalid_g2(), 1));
}


fun main() {
}

/**
Low-level BLS opcodes do not guarantee subgroup validation. ADD/SUB have
asymmetric `blst aggregate()` implementation details, so these cases still
throw 11 for the validated operand direction.

@testcase | 100 |    | 0
@testcase | 101 |    | 0
@testcase | 102 |    | 0
@testcase | 103 |    | 0
@testcase | 104 |    | 0
@testcase | 105 |    | 0
@testcase | 106 |    | 0
@testcase | 107 |    | 11
@testcase | 108 |    | 11

@testcase | 200 |    | -1
@testcase | 201 |    | -1
@testcase | 202 |    | -1
@testcase | 203 |    | -1
@testcase | 204 |    | -1
@testcase | 205 |    | -1
@testcase | 206 |    | 0

High-level verification opcodes reject invalid subgroup inputs by returning false.
BLS_AGGREGATE is only an aggregation helper, not a verifier; for one signature it
does not guarantee subgroup validation by itself.

@testcase | 300 |    | -1
@testcase | 301 |    | 0
@testcase | 302 |    | 0
@testcase | 303 |    | -1
@testcase | 304 |    | 0
@testcase | 305 |    | 0
@testcase | 306 |    | -1
@testcase | 307 |    | 0
@testcase | 308 |    | 0
@testcase | 309 |    | -1
@testcase | 310 |    | 0
 */
