
fun rist255_validate(x: int): void
    asm "RIST255_VALIDATE"
fun rist255_qvalidate(x: int): bool
    asm "RIST255_QVALIDATE"
fun rist255_add(x: int, y: int): int
    asm "RIST255_ADD"
fun rist255_sub(x: int, y: int): int
    asm "RIST255_SUB"
fun rist255_mul(x: int, n: int): int
    asm "RIST255_MUL"
fun rist255_qmul(x: int, n: int): (int, bool)       // it's `(int, -1)` on positive only
    asm "RIST255_QMUL"
fun rist255_mulbase(n: int): int
    asm "RIST255_MULBASE"
fun rist255_qmulbase(n: int): (int, bool)           // it's `(int, -1)` on positive only
    asm "RIST255_QMULBASE"
fun rist255_pushl(): int
    asm "RIST255_PUSHL"
fun ecrecover(hash: int, v: int, r: int, s: int): (int, int, int, bool)
    asm "ECRECOVER"
fun ed25519_chksignu(hash: int, signature: slice, publicKey: int): bool
    asm "CHKSIGNU"
fun ed25519_chksigns(data: slice, signature: slice, publicKey: int): bool
    asm "CHKSIGNS"

// Value of `RIST255_MULBASE 1`
const BASEPOINT = 102651481954198948695408991041606107487729423467545670950322577403614277217654

// 2 * basepoint
const TWO_BASEPOINT = 48074487330273963923922157365918574336273011965350013600919792151764305951001

// One of the canonical "bad encoding" (off-curve) vectors from rist255.fif
const INVALID_RIST_POINT = 0xc34c4e1826e5d403b78e246e88aa051c36ccf0aafebffe137d148a2bf9104562

const ECRECOVER_RECID0_HASH = 0x0000000000000000000000000000000000000000000000000000000000000001
const ECRECOVER_RECID0_R = 0x64a8d9bad3b13101867f1c9a138643bfc552a7723c40b7d1d998d9848ed500db
const ECRECOVER_RECID0_S = 0x131305e5c11a6e47e82a0d17b8bc6c6558db075b2c54ff21ee38101e8b54111a

const ECRECOVER_RECID1_HASH = 0x546869732069732061207665727920736563726574206d6573736167652e2e2e
const ECRECOVER_RECID1_R = 0x67cb285f9cd194e840d629397af5569662fde44649995963179a7dd17bd23532
const ECRECOVER_RECID1_S = 0x4b1b7df34ce1f68e694ff6f11ac751dd7dd73e387ee4fc866e1be8ecc7dd9557

// Canonical Ed25519 identity public key encoding `01 00..00`
const ED25519_IDENTITY_PUBKEY = 0x0100000000000000000000000000000000000000000000000000000000000000
// Forged 64-byte signature with `R = O` (`01 00..00`) and `S = 0`. Together with the identity public key
// this satisfies `S*B = R + H(R,A,M)*A` for any message under permissive Ed25519 verifiers
const ED25519_IDENTITY_FORGED_SIG = "01000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000".hexToSlice()
// message `af82` signed by the corresponding key
const ED25519_RFC8032_PUBKEY = 0xfc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025
const ED25519_RFC8032_MSG = "af82".hexToSlice()
const ED25519_RFC8032_SIG = "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a".hexToSlice()

fun validate_excno(x: int): int {
    try {
        rist255_validate(x);
        return 0;
    } catch (excno) {
        return excno;
    }
}

fun mul_excno(x: int, n: int): int {
    try {
        rist255_mul(x, n);
        return 0;
    } catch (excno) {
        return excno;
    }
}


@method_id(101)
fun test_mulbase() {
    return (
        rist255_mulbase(0),
        rist255_mulbase(1),
        rist255_mulbase(2),
        rist255_mulbase(rist255_pushl()),  // n = l -> n % l == 0 -> identity
    );
}

@method_id(102)
fun test_qvalidate() {
    return (
        rist255_qvalidate(BASEPOINT),
        rist255_qvalidate(0),
    );
}

@method_id(103)
fun test_mul() {
    return (
        rist255_mul(BASEPOINT, 0),
        rist255_mul(BASEPOINT, 1),
        rist255_mul(BASEPOINT, 2),
        rist255_mul(BASEPOINT, rist255_pushl()),
        rist255_mul(0, 100500),
    );
}

@method_id(104)
fun test_add_sub() {
    val neg_basepoint = rist255_mul(BASEPOINT, rist255_pushl() - 1);
    return (
        rist255_add(BASEPOINT, neg_basepoint),
        rist255_sub(BASEPOINT, BASEPOINT),
    );
}

@method_id(105)
fun test_validate() {
    return (
        validate_excno(BASEPOINT),
        validate_excno(0),
        validate_excno(INVALID_RIST_POINT),
    );
}

@method_id(106)
fun test_qmul(): (int, bool) {
    return rist255_qmul(0, 1);
}

@method_id(107)
fun test_qmulbase() {
    val (r_zero, ok_zero) = rist255_qmulbase(0);
    val (r_l,    ok_l)    = rist255_qmulbase(rist255_pushl());
    val (r_bp,   ok_bp)   = rist255_qmulbase(BASEPOINT);
    return (r_zero, ok_zero, r_l, ok_l, r_bp, ok_bp);
}

@method_id(108)
fun test_mul_validates_x() {
    return (
        mul_excno(INVALID_RIST_POINT, 0),
        mul_excno(INVALID_RIST_POINT, rist255_pushl()),
        mul_excno(INVALID_RIST_POINT, 1),
        mul_excno(1, INVALID_RIST_POINT),
        mul_excno(BASEPOINT, 0),
        mul_excno(0, 0),
    );
}

@method_id(109)
fun test_ecrecover_v0(): (int, int, int, bool) {
    return ecrecover(ECRECOVER_RECID0_HASH, 0, ECRECOVER_RECID0_R, ECRECOVER_RECID0_S);
}

@method_id(110)
fun test_ecrecover_v3(): (int, int, int, bool) {
    return ecrecover(ECRECOVER_RECID0_HASH, 3, ECRECOVER_RECID0_R, ECRECOVER_RECID0_S);
}

@method_id(111)
fun test_ecrecover_v27(): (int, int, int, bool) {
    return ecrecover(ECRECOVER_RECID0_HASH, 27, ECRECOVER_RECID0_R, ECRECOVER_RECID0_S);
}

@method_id(112)
fun test_ecrecover_v28(): (int, int, int, bool) {
    return ecrecover(ECRECOVER_RECID1_HASH, 28, ECRECOVER_RECID1_R, ECRECOVER_RECID1_S);
}

@method_id(113)
fun test_ecrecover_v123(): (int, int, int, bool) {
    return ecrecover(ECRECOVER_RECID0_HASH, 123, ECRECOVER_RECID0_R, ECRECOVER_RECID0_S);
}

@method_id(116)
fun test_chksigns_identity_pubkey_forgery(): bool {
    return ed25519_chksigns(ED25519_RFC8032_MSG, ED25519_IDENTITY_FORGED_SIG, ED25519_IDENTITY_PUBKEY);
}

@method_id(117)
fun test_chksignu_identity_pubkey_forgery(): bool {
    return ed25519_chksignu(0, ED25519_IDENTITY_FORGED_SIG, ED25519_IDENTITY_PUBKEY);
}

@method_id(118)
fun test_chksigns_valid_rfc8032(): bool {
    return ed25519_chksigns(ED25519_RFC8032_MSG, ED25519_RFC8032_SIG, ED25519_RFC8032_PUBKEY);
}


fun main() {}

/**
@testcase | 101 |    | 0 102651481954198948695408991041606107487729423467545670950322577403614277217654 48074487330273963923922157365918574336273011965350013600919792151764305951001 0
@testcase | 102 |    | -1 -1
@testcase | 103 |    | 0 102651481954198948695408991041606107487729423467545670950322577403614277217654 48074487330273963923922157365918574336273011965350013600919792151764305951001 0 0
@testcase | 104 |    | 0 0
@testcase | 105 |    | 0 0 5
@testcase | 106 |    | 0 -1
@testcase | 107 |    | 0 -1 0 -1 98738712007019247557047908298095763791962011080994539901560598933493739733353 -1
@testcase | 108 |    | 5 5 5 5 0 0
@testcase | 109 |    | 4 103388573995635080359749164254216598308788835304023601477803095234286494993683 37057141145242123013015316630864329550140216928701153669873286428255828810018 -1
@testcase | 110 |    | 0
@testcase | 111 |    | 4 103388573995635080359749164254216598308788835304023601477803095234286494993683 37057141145242123013015316630864329550140216928701153669873286428255828810018 -1
@testcase | 112 |    | 4 60848959681706811729418709042689483600228987410573073530302635708531648453916 101881881594906628349373666322566678393963977859535209354218873515872455785045 -1
@testcase | 113 |    | 0
@testcase | 116 |    | 0
@testcase | 117 |    | 0
@testcase | 118 |    | -1
 */
