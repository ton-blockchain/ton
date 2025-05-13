// this is actually `mathlib.fc` transformed to Tolk

import "@stdlib/tvm-lowlevel"

/*--------------- MISSING OPERATIONS AND BUILT-INS ----------------*/

/// compute floor(log2(x))+1
@pure
fun log2_floor_p1(x: int): int
    asm "UBITSIZE";

@pure
fun mulrshiftr(x: int, y: int, s: int): int
    asm "MULRSHIFTR";

@pure
fun mulrshiftr256(x: int, y: int): int
    asm "256 MULRSHIFTR#";

@pure
fun mulrshift256mod(x: int, y: int): (int, int)
    asm "256 MULRSHIFT#MOD";

@pure
fun mulrshiftr256mod(x: int, y: int): (int, int)
    asm "256 MULRSHIFTR#MOD";

@pure
fun mulrshiftr255mod(x: int, y: int): (int, int)
    asm "255 MULRSHIFTR#MOD";

@pure
fun mulrshiftr248mod(x: int, y: int): (int, int)
    asm "248 MULRSHIFTR#MOD";

@pure
fun mulrshiftr5mod(x: int, y: int): (int, int)
    asm "5 MULRSHIFTR#MOD";

@pure
fun mulrshiftr6mod(x: int, y: int): (int, int)
    asm "6 MULRSHIFTR#MOD";

@pure
fun mulrshiftr7mod(x: int, y: int): (int, int)
    asm "7 MULRSHIFTR#MOD";

@pure
fun lshift256divr(x: int, y: int): int
    asm "256 LSHIFT#DIVR";

@pure
fun lshift256divmodr(x: int, y: int): (int, int)
    asm "256 LSHIFT#DIVMODR";

@pure
fun lshift255divmodr(x: int, y: int): (int, int)
    asm "255 LSHIFT#DIVMODR";

@pure
fun lshift2divmodr(x: int, y: int): (int, int)
    asm "2 LSHIFT#DIVMODR";

@pure
fun lshift7divmodr(x: int, y: int): (int, int)
    asm "7 LSHIFT#DIVMODR";

@pure
fun lshiftdivmodr(x: int, y: int, s: int): (int, int)
    asm "LSHIFTDIVMODR";

@pure
fun rshiftr256mod(x: int): (int, int)
    asm "256 RSHIFTR#MOD";

@pure
fun rshiftr248mod(x: int): (int, int)
    asm "248 RSHIFTR#MOD";

@pure
fun rshiftr4mod(x: int): (int, int)
    asm "4 RSHIFTR#MOD";

@pure
fun rshift3mod(x: int): (int, int)
    asm "3 RSHIFT#MOD";

/// computes y - x (Tolk compiler does not try to use this by itself)
@pure
fun sub_rev(x: int, y: int): int
    asm "SUBR";

@pure
fun nan(): int
    asm "PUSHNAN";

@pure
fun is_nan(x: int): int
    asm "ISNAN";

/*----------------------- SQUARE ROOTS ---------------------------*/

/// computes sqrt(a*b) exactly rounded to the nearest integer
/// for all 0 <= a, b <= 2^256-1
/// may be used with b=1 or b=scale of fixed-point numbers
@pure
@inline_ref
fun geom_mean(a: int, b: int): int {
    if (!min(a, b)) {
    return 0;
    }
    var s: int = log2_floor_p1(a);   // throws out of range error if a < 0 or b < 0
    var t: int = log2_floor_p1(b);
    // NB: (a-b)/2+b == (a+b)/2, but without overflow for large a and b
    var x: int = (s == t ? (a - b) / 2 + b : 1 << ((s + t) / 2));
    do {
    // if always used with b=2^const, may be optimized to "const LSHIFTDIVC#"
    // it is important to use `mulDivCeil` here, not `mulDivFloor` or `mulDivRound`
    var q: int = (mulDivCeil(a, b, x) - x) / 2;
    x += q;
    } while (q);
    return x;
}

/// integer square root, computes round(sqrt(a)) for all a>=0.
/// note: `inline` is better than `inline_ref` for such simple functions
@pure
@inline
fun sqrt(a: int): int {
    return geom_mean(a, 1);
}

/// version for fixed248 = fixed-point numbers with scale 2^248
/// fixed248 sqrt(fixed248 x)
@pure
@inline
fun fixed248_sqrt(x: int): int {
    return geom_mean(x, 1 << 248);
}

/// fixed255 sqrt(fixed255 x)
@pure
@inline
fun fixed255_sqrt(x: int): int {
    return geom_mean(x, 1 << 255);
}

/// fixed248 sqr(fixed248 x);
@pure
@inline
fun fixed248_sqr(x: int): int {
    return mulDivRound(x, x, 1 << 248);
}

/// fixed255 sqr(fixed255 x);
@pure
@inline
fun fixed255_sqr(x: int): int {
    return mulDivRound(x, x, 1 << 255);
}

const fixed248_One: int = (1 << 248);
const fixed255_One: int = (1 << 255);

/*------------------- USEFUL CONSTANTS -------------------*/

/// store huge constants in inline_ref functions for reuse
/// (y,z) where y=round(log(2)*2^256), z=round((log(2)*2^256-y)*2^128)
/// then log(2) = y/2^256 + z/2^384
@pure
@inline_ref
fun log2_xconst_f256(): (int, int) {
    return (80260960185991308862233904206310070533990667611589946606122867505419956976172, -32272921378999278490133606779486332143);
}

/// (y,z) where Pi = y/2^254 + z/2^382
@pure
@inline_ref
fun Pi_xconst_f254(): (int, int) {
    return (90942894222941581070058735694432465663348344332098107489693037779484723616546, 108051869516004014909778934258921521947);
}

/// atan(1/16) as fixed260
@pure
@inline_ref
fun Atan1_16_f260(): int {
    return 115641670674223639132965820642403718536242645001775371762318060545014644837101;  // true value is ...101.0089...
}

/// atan(1/8) as fixed259
@pure
@inline_ref
fun Atan1_8_f259(): int {
    return 115194597005316551477397594802136977648153890007566736408151129975021336532841;  // correction -0.1687...
}

/// atan(1/32) as fixed261
@pure
@inline_ref
fun Atan1_32_f261(): int {
    return 115754418570128574501879331591757054405465733718902755858991306434399246026247;  // correction 0.395...
}

/// inline is better than inline_ref for such very small functions
@pure
@inline
fun log2_const_f256(): int {
    var (c: int, _) = log2_xconst_f256();
    return c;
}

@pure
@inline
fun fixed248_log2_const(): int {
    return log2_const_f256() ~>> 8;
}

@pure
@inline
fun Pi_const_f254(): int {
    var (c, _) = Pi_xconst_f254();
    return c;
}

@pure
@inline
fun fixed248_Pi_const(): int {
    return Pi_const_f254() ~>> 6;
}

/*-------------- HYPERBOLIC TANGENT AND EXPONENT ------------------*/

/// hyperbolic tangent of small x via n+2 terms of Lambert's continued fraction
/// n=17: good for |x| < log(2)/4 = 0.173
/// fixed258 tanh_f258(fixed258 x, int n)
@pure
@inline_ref
fun tanh_f258(x: int, n: int): int {
    var x2: int = mulDivRound(x, x, 1 << 255);     // x^2 as fixed261
    var a: int = (2 * n + 5) << 250;   // a=2n+5 as fixed250
    var c = a;
    var Two: int = (1 << 251);   // 2. as fixed250
    repeat (n) {
        a = (c -= Two) + mulDivRound(x2, 1 << 239, a);      // a := 2k+1+x^2/a as fixed250, k=n+1,n,...,2
    }
    a = (3 << 254) + mulDivRound(x2, 1 << 243, a);    // a := 3+x^2/a as fixed254
    // y = x/(1+a') = x - x*a'/(1+a') = x - x*x^2/(a+x^2)  where a' = x^2/a
    return x - (mulDivRound(x, x2, a + (x2 ~>> 7)) ~>> 7);
}

/// fixed257 expm1_f257(fixed257 x)
/// computes exp(x)-1 for small x via 19 terms of Lambert's continued fraction for tanh(x/2)
/// good for |x| < log(2)/2 = 0.347 (n=17); consumes ~3500 gas
@pure
@inline_ref
fun expm1_f257(x: int): int {
    // (almost) compute tanh(x/2) first; x/2 as fixed258 = x as fixed257
    var x2: int = mulDivRound(x, x, 1 << 255);     // x^2 as fixed261
    var Two: int = (1 << 251);   // 2. as fixed250
    var a: int = 39 << 250;   // a=2n+5 as fixed250
    var c = a;
    repeat (17) {
        a = (c -= Two) + mulDivRound(x2, 1 << 239, a);      // a := 2k+1+x^2/a as fixed250, k=n+1,n,...,2
    }
    a = (3 << 254) + mulDivRound(x2, 1 << 243, a);    // a := 3+x^2/a as fixed254
    // now tanh(x/2) = x/(1+a') where a'=x^2/a ; apply exp(x)-1=2*tanh(x/2)/(1-tanh(x/2))
    var t: int = (x ~>> 4) - a;   // t:=x-a as fixed254
    return x - mulDivRound(x2, t / 2, a + mulrshiftr256(x, t) ~/ 4) ~/ 4;  // x - x^2 * (x-a) / (a + x*(x-a))
}

/// expm1_f257() may be used to implement specific fixed-point exponentials
/// example:
/// fixed248 exp(fixed248 x)
@pure
@inline_ref
fun fixed248_exp(x: int): int {
    var (l2c, l2d) = log2_xconst_f256();
    // divide x by log(2) and convert to fixed257
    // (int q, x) = muldivmodr(x, 256, l2c);  // unfortunately, no such built-in
    var (q: int, x redef) = lshiftdivmodr(x, l2c, 8);
    x = 2 * x - mulDivRound(q, l2d, 1 << 127);
    var y: int = expm1_f257(x);
    // result is (1 + y) * (2^q) --> ((1 << 257) + y) >> (9 - q)
    return (y ~>> (9 - q)) - (-1 << (248 + q));
    // note that (y ~>> (9 - q)) + (1 << (248 + q)) leads to overflow when q=8
}

/// compute 2^x in fixed248
/// fixed248 exp2(fixed248 x)
@pure
@inline_ref
fun fixed248_exp2(x: int): int {
    // (int q, x) = divmodr(x, 1 << 248);   // no such built-in
    var (q: int, x redef) = rshiftr248mod(x);
    x = mulDivRound(x, log2_const_f256(), 1 << 247);
    var y: int = expm1_f257(x);
    return (y ~>> (9 - q)) - (-1 << (248 + q));
}

/*-------------------- TRIGONOMETRIC FUNCTIONS ----------------------*/

/// fixed260 tan(fixed260 x);
/// computes tan(x) for small |x|<atan(1/16) via 16 terms of Lambert's continued fraction
@pure
@inline
fun tan_f260_inlined(x: int): int {
    var x2: int = mulrshiftr256(x, x);     // x^2 as fixed264
    var Two: int = (1 << 251);   // 2. as fixed250
    var a: int = 33 << 250;   // a=2n+5 as fixed250
    var c = a;
    repeat (14) {
        a = (c -= Two) - mulDivRound(x2, 1 << 236, a);      // a := 2k+1-x^2/a as fixed250, k=n+1,n,...,2
    }
    a = (3 << 254) - mulDivRound(x2, 1 << 240, a);    // a := 3-x^2/a as fixed254
    // y = x/(1-a') = x + x*a'/(1-a') = x + x*x^2/(a-x^2)  where a' = x^2/a
    return x + (mulDivRound(x / 2, x2, a - (x2 ~>> 10)) ~>> 9);
}

/// fixed260 tan(fixed260 x);
@pure
@inline_ref
fun tan_f260(x: int): int {
    return tan_f260_inlined(x);
}

/// fixed258 tan(fixed258 x);
/// computes tan(x) for small |x|<atan(1/4) via 20 terms of Lambert's continued fraction
@pure
@inline
fun tan_f258_inlined(x: int): int {
    var x2: int = mulrshiftr256(x, x);     // x^2 as fixed260
    var Two: int = (1 << 251);   // 2. as fixed250
    var a: int = 41 << 250;   // a=2n+5 as fixed250
    var c = a;
    repeat (18) {
        a = (c -= Two) - mulDivRound(x2, 1 << 240, a);      // a := 2k+1-x^2/a as fixed250, k=n+1,n,...,2
    }
    a = (3 << 254) - mulDivRound(x2, 1 << 244, a);    // a := 3-x^2/a as fixed254
    // y = x/(1-a') = x + x*a'/(1-a') = x + x*x^2/(a-x^2)  where a' = x^2/a
    return x + (mulDivRound(x / 2, x2, a - (x2 ~>> 6)) ~>> 5);
}

/// fixed258 tan(fixed258 x);
@pure
@inline_ref
fun tan_f258(x: int): int {
    return tan_f258_inlined(x);
}

/// (fixed259, fixed263) sincosm1(fixed259 x)
/// computes (sin(x), 1-cos(x)) for small |x|<2*atan(1/16)
@pure
@inline
fun sincosm1_f259_inlined(x: int): (int, int) {
    var t: int = tan_f260_inlined(x);   // t=tan(x/2) as fixed260
    var tt: int = mulrshiftr256(t, t);  // t^2 as fixed264
    var y: int = tt ~/ 512 + (1 << 255);   // 1+t^2 as fixed255
    // 2*t/(1+t^2) as fixed259 and 2*t^2/(1+t^2) as fixed263
    // return (mulDivRound(t, 1 << 255, y), mulDivRound(tt, 1 << 255, y));
    return (t - mulDivRound(t / 2, tt, y) ~/ 256, tt - mulDivRound(tt / 2, tt, y) ~/ 256);
}

@pure
@inline_ref
fun sincosm1_f259(x: int): (int, int) {
    return sincosm1_f259_inlined(x);
}

/// computes (sin(x+xe),-cos(x+xe)) for |x| <= Pi/4, xe very small
/// this function is very accurate, error less than 0.7 ulp (consumes ~ 5500 gas)
/// (fixed256, fixed256) sincosn(fixed256 x, fixed259 xe)
@pure
@inline_ref
fun sincosn_f256(x: int, xe: int): (int, int) {
    // var (q, x1) = muldivmodr(x, 8, Atan1_8_f259());  // no muldivmodr() builtin
    var (q, x1) = lshift2divmodr(abs(x), Atan1_8_f259());  // reduce mod theta where theta=2*atan(1/8)
    var (si, co) = sincosm1_f259(x1 * 2 + xe);
    var (a, b, c) = (-1, 0, 1);
    repeat (q) {
        // (a+b*I) *= (8+I)^2 = 63+16*I
        (a, b, c) = (63 * a - 16 * b, 16 * a + 63 * b, 65 * c);
    }
    // now a/c = cos(q*theta), b/c = sin(q*theta) exactly(!)
    // compute (a+b*I)*(1-co+si*I)/c
    // (b, a) = (lshift256divr(b, c), lshift256divr(a, c));
    var (b redef, br: int) = lshift256divmodr(b, c);  br = mulDivRound(br, 128, c);
    var (a redef, ar: int) = lshift256divmodr(a, c);  ar = mulDivRound(ar, 128, c);
    return (sign(x) * (((mulrshiftr256(b, co) - br) ~/ 16 - mulrshiftr256(a, si)) ~/ 8 - b),
    a - ((mulrshiftr256(a, co) - ar) ~/ 16 + mulrshiftr256(b, si)) ~/ 8);
}

/// compute (sin(x),1-cos(x)) in fixed256 for |x| < 16*atan(1/16) = 0.9987
/// (fixed256, fixed257) sincosm1_f256(fixed256 x);
/// slightly less accurate than sincosn_f256() (error up to 3/2^256), but faster (~ 4k gas) and shorter
@pure
@inline_ref
fun sincosm1_f256(x: int): (int, int) {
    var (si, co) = sincosm1_f259_inlined(x);   // compute (sin,1-cos)(x/8) in (fixed259,fixed263)
    var r: int = 7;
    repeat (r / 2) {
        // 1-cos(2*x) = 2*sin(x)^2, sin(2*x) = 2*sin(x)*cos(x)
        (co, si) = (mulrshiftr256(si, si), si - (mulrshiftr256(si, co) ~>> r));
        r -= 2;
    }
    return (si, co);
}

/// compute (p, q) such that p/q = tan(x) for |x|<2*atan(1/2)=1899/2048=0.927
/// (int, int) tan_aux(fixed256 x);
@pure
@inline_ref
fun tan_aux_f256(x: int): (int, int) {
    var t: int = tan_f258_inlined(x);   // t=tan(x/4) as fixed258
    // t:=2*t/(1-t^2)=2*(t-t^3/(t^2-1))
    var tt: int = mulrshiftr256(t, t);  // t^2 as fixed260
    t = mulDivRound(t, tt, tt ~/ 16 + (-1 << 256)) ~/ 16 - t;   // now t=-tan(x/2) as fixed259
    return (t, mulrshiftr256(t, t) ~/ 4 + (-1 << 256));  // return (2*t, t^2-1) as fixed256
}

/// sincosm1_f256() and sincosn_f256() may be used to implement trigonometric functions for different fixed-point types
/// example:
/// (fixed248, fixed248) sincos(fixed248 x);
@pure
@inline_ref
fun fixed248_sincos(x: int): (int, int) {
    var (Pic, Pid) = Pi_xconst_f254();
    // (int q, x) = muldivmodr(x, 128, Pic);  // no muldivmodr() builtin
    var (q: int, x redef) = lshift7divmodr(x, Pic);  // reduce mod Pi/2
    x = 2 * x - mulDivRound(q, Pid, 1 << 127);
    var (si: int, co: int) = sincosm1_f256(x);   // doesn't make sense to use more accurate sincosn_f256()
    co = (1 << 248) - (co ~>> 9);
    si = si ~>> 8;
    repeat (q & 3) {
    (si, co) = (co, -si);
    }
    return (si, co);
}

/// fixed248 sin(fixed248 x);
/// inline is better than inline_ref for such simple functions
@pure
@inline
fun fixed248_sin(x: int): int {
    var (si: int, _) = fixed248_sincos(x);
    return si;
}

/// fixed248 cos(fixed248 x);
@pure
@inline
fun fixed248_cos(x: int): int {
    var (_, co: int) = fixed248_sincos(x);
    return co;
}

/// similarly, tan_aux_f256() may be used to implement tan() and cot() for specific fixed-point formats
/// fixed248 tan(fixed248 x);
/// not very accurate when |tan(x)| is very large (difficult to do better without floating-point numbers)
/// however, the relative accuracy is approximately 2^-247 in all cases, which is good enough for arguments given up to 2^-249
@pure
@inline_ref
fun fixed248_tan(x: int): int {
    var (Pic, Pid) = Pi_xconst_f254();
    // (int q, x) = muldivmodr(x, 128, Pic);  // no muldivmodr() builtin
    var (q: int, x redef) = lshift7divmodr(x, Pic);  // reduce mod Pi/2
    x = 2 * x - mulDivRound(q, Pid, 1 << 127);
    var (a, b) = tan_aux_f256(x);  // now a/b = tan(x')
    if (q & 1) {
    (a, b) = (b, -a);
    }
    return mulDivRound(a, 1 << 248, b);   // either -b/a or a/b as fixed248
}

/// fixed248 cot(fixed248 x);
@pure
@inline_ref
fun fixed248_cot(x: int): int {
    var (Pic, Pid) = Pi_xconst_f254();
    var (q: int, x redef) = lshift7divmodr(x, Pic);  // reduce mod Pi/2
    x = 2 * x - mulDivRound(q, Pid, 1 << 127);
    var (b, a) = tan_aux_f256(x);  // now b/a = tan(x')
    if (q & 1) {
    (a, b) = (b, -a);
    }
    return mulDivRound(a, 1 << 248, b);   // either -b/a or a/b as fixed248
}

/*---------------- INVERSE HYPERBOLIC TANGENT AND LOGARITHMS ----------------*/

/// inverse hyperbolic tangent of small x, evaluated by means of n terms of the continued fraction
/// valid for |x| < 2^-2.5 ~ 0.18 if n=37 (slightly less accurate with n=36)
/// |x| < 1/8 if n=32;  |x| < 2^-3.5 if n=28;  |x| < 1/16 if n=25
/// |x| < 2^-4.5 if n=23;  |x| < 1/32 if n=21;  |x| < 1/64 if n=18
/// fixed258 atanh(fixed258 x);
@pure
@inline_ref
fun atanh_f258(x: int, n: int): int {
    var x2: int = mulrshiftr256(x, x);  // x^2 as fixed260
    var One: int = (1 << 254);
    var a: int = One ~/ n + (1 << 255);  // a := 2 + 1/n as fixed254
    repeat (n - 1) {
        // a := 1 + (1 - x^2 / a)(1 + 1/n) as fixed254
        var t: int = One - mulDivRound(x2, 1 << 248, a);    // t := 1 - x^2 / a
        var n1: int = n - 1;
        a = mulDivRound(t, n, n1) + One;
        n = n1;
    }
    // x / (1 - x^2 / a) = x / (1 - d) = x + x * d / (1 - d) for d = x^2 / a
    // int d = mulDivRound(x2, 1 << 255, a - (x2 ~>> 6));  // d/(1-d) = x^2/(a-x^2) as fixed261
    // return x + (mulrshiftr256(x, d) ~>> 5);
    return x + mulDivRound(x, x2 / 2, a - x2 ~/ 64) ~/ 32;
}

/// number of terms n should be chosen as for atanh_f258()
/// fixed261 atanh(fixed261 x);
@pure
@inline
fun atanh_f261_inlined(x: int, n: int): int {
    var x2: int = mulrshiftr256(x, x);  // x^2 as fixed266
    var One: int = (1 << 254);
    var a: int = One ~/ n + (1 << 255);  // a := 2 + 1/n as fixed254
    repeat (n - 1) {
        // a := 1 + (1 - x^2 / a)(1 + 1/n) as fixed254
        var t: int = One - mulDivRound(x2, 1 << 242, a);    // t := 1 - x^2 / a
        var n1: int = n - 1;
        a = mulDivRound(t, n, n1) + One;
        n = n1;
    }
    // x / (1 - x^2 / a) = x / (1 - d) = x + x * d / (1 - d) for d = x^2 / a
    // int d = mulDivRound(x2, 1 << 255, a - (x2 ~>> 12));  // d/(1-d) = x^2/(a-x^2) as fixed267
    // return x + (mulrshiftr256(x, d) ~>> 11);
    return x + mulDivRound(x, x2, a - x2 ~/ 4096) ~/ 4096;
}

/// fixed261 atanh(fixed261 x);
@pure
@inline_ref
fun atanh_f261(x: int, n: int): int {
    return atanh_f261_inlined(x, n);
}

/// returns (y, s) such that log(x) = y/2^257 + s*log(2) for positive integer x
/// (fixed257, int) log_aux(int x)
@pure
@inline_ref
fun log_aux_f257(x: int): (int, int) {
    var s: int = log2_floor_p1(x);
    x <<= 256 - s;
    var t: int = -1 << 256;
    if ((x >> 249) <= 90) {
        t >>= 1;
        s -= 1;
    }
    x += t;
    var `2x`: int = 2 * x;
    var y: int = lshift256divr(`2x`, (x >> 1) - t);
    // y = `2x` - (mulrshiftr256(2x, y) ~>> 2);  // this line could improve precision on very rare occasions
    return (atanh_f258(y, 36), s);
}

/// computes 33^m for small m
@pure
@inline
fun pow33(m: int): int {
    var t: int = 1;
    repeat (m) {
        t *= 33;
    }
    return t;
}

/// computes 33^m for small 0<=m<=22
/// slightly faster than pow33()
@pure
@inline
fun pow33b(m: int): int {
    var (mh: int, ml: int) = divMod(m, 5);
    var t: int = 1;
    repeat (ml) {
        t *= 33;
    }
    repeat (mh) {
        t *= 33 * 33 * 33 * 33 * 33;
    }
    return t;
}

/// returns (s, q, y) such that log(x) = s*log(2) + q*log(33/32) + y/2^260 for positive integer x
/// (int, int, fixed260) log_auxx_f260(int x);
@pure
@inline_ref
fun log_auxx_f260(x: int): (int, int, int) {
    var s: int = log2_floor_p1(x) - 1;
    x <<= 255 - s;   // rescale to 1 <= x < 2 as fixed255
    var t: int = 2873 << 244;   // ~ (33/32)^11 ~ sqrt(2) as fixed255
    var x1: int = (x - t) >> 1;
    var q: int = mulDivRound(x1, 65, x1 + t) + 11;   // crude approximation to round(log(x)/log(33/32))
    // t = 1;  repeat (q) { t *= 33; }  // t:=33^q, 0<=q<=22
    t = pow33b(q);
    t <<= (51 - q) * 5;   // t:=(33/32)^q as fixed255, nearest power of 33/32 to x
    x -= t;
    var y: int = lshift256divr(x << 4, (x >> 1) + t);    // y = (x-t)/(x+t) as fixed261
    y = atanh_f261(y, 18);   // atanh((x-t)/(x+t)) as fixed261, or log(x/t) as fixed260
    return (s, q, y);
}

/// returns (y, s) such that log(x) = y/2^256 + s*log(2) for positive integer x
/// this function is very precise (error less than 0.6 ulp) and consumes < 7k gas
/// may be used to implement specific fixed-point instances of log() and log2()
/// (fixed256, int) log_aux_f256(int x);
@pure
@inline_ref
fun log_aux_f256(x: int): (int, int) {
    var (s, q, y) = log_auxx_f260(x);
    var (yh, yl) = rshiftr4mod(y);   // y ~/% 16 , but Tolk does not optimize this to RSHIFTR#MOD
    // int Log33_32 = 3563114646320977386603103333812068872452913448227778071188132859183498739150;  // log(33/32) as fixed256
    // int Log33_32_l = -3769;   // log(33/32) = Log33_32 / 2^256 + Log33_32_l / 2^269
    yh += (yl * 512 + q * -3769) ~>> 13;   // compensation, may be removed if slightly worse accuracy is acceptable
    var Log33_32: int = 3563114646320977386603103333812068872452913448227778071188132859183498739150;  // log(33/32) as fixed256
    return (yh + q * Log33_32, s);
}

/// returns (y, s) such that log2(x) = y/2^256 + s for positive integer x
/// this function is very precise (error less than 0.6 ulp) and consumes < 7k gas
/// may be used to implement specific fixed-point instances of log() and log2()
/// (fixed256, int) log2_aux_f256(int x);
@pure
@inline_ref
fun log2_aux_f256(x: int): (int, int) {
    var (s, q, y) = log_auxx_f260(x);
    y = lshift256divr(y, log2_const_f256()) ~>> 4;  // y/log(2) as fixed256
    var Log33_32: int = 5140487830366106860412008603913034462883915832139695448455767612111363481357;  // log_2(33/32) as fixed256
    // Log33_32/2^256 happens to be a very precise approximation to log_2(33/32), no compensation required
    return (y + q * Log33_32, s);
}


/// fixed248 log(fixed248 x)
@pure
@inline_ref
fun fixed248_log(x: int): int {
    var (y, s) = log_aux_f256(x);
    return mulDivRound(s - 248, log2_const_f256(), 1 << 8) + (y ~>> 8);
    // return mulDivRound(s - 248, 80260960185991308862233904206310070533990667611589946606122867505419956976172, 1 << 8) + (y ~>> 8);
}

/// fixed248 log2(fixed248 x)
@pure
@inline
fun fixed248_log2(x: int): int {
    var (y, s) = log2_aux_f256(x);
    return ((s - 248) << 248) + (y ~>> 8);
}

/// computes x^y as exp(y*log(x)), x >= 0
/// fixed248 pow(fixed248 x, fixed248 y);
@pure
@inline_ref
fun fixed248_pow(x: int, y: int): int {
    if (!y) {
    return 1 << 248;   // x^0 = 1
    }
    if (x <= 0) {
    var bad: int = ((x | y) < 0) as int;
    return 0 >> bad;          // 0^y = 0 if x=0 and y>=0; "out of range" exception otherwise
    }
    var (l, s) = log2_aux_f256(x);
    s -= 248;   // log_2(x) = s+l, l is fixed256, 0<=l<1
    // compute (s+l)*y = q+ll
    var (q1, r1) = mulrshiftr248mod(s, y);   // muldivmodr(s, y, 1 << 248)
    var (q2, r2) = mulrshift256mod(l, y);
    r2 >>= 247;
    var (q3, r3) = rshiftr248mod(q2);        // divmodr(q2, 1 << 248);
    var (q, ll) = rshiftr248mod(r1 + r3);
    ll = 512 * ll + r2;
    q += q1 + q3;
    // now log_2(x^y) = y*log_2(x) = q + ll, ss integer, ll fixed257, -1/2<=ll<1/2
    var sq: int = q + 248;
    if (sq <= 0) {
    return -((sq == 0) as int);   // underflow
    }
    y = expm1_f257(mulrshiftr256(ll, log2_const_f256()));
    return (y ~>> (9 - q)) - (-1 << sq);
}

/*-------------------- INVERSE TRIGONOMETRIC FUNCTIONS ------------------*/

/// number of terms n should be chosen as for atanh_f258()
/// fixed259 atan(fixed259 x);
@pure
@inline_ref
fun atan_f259(x: int, n: int): int {
    var x2: int = mulrshiftr256(x, x);  // x^2 as fixed262
    var One: int = (1 << 254);
    var a: int = One ~/ n + (1 << 255);  // a := 2 + 1/n as fixed254
    repeat (n - 1) {
        // a := 1 + (1 + x^2 / a)(1 + 1/n) as fixed254
        var t: int = One + mulDivRound(x2, 1 << 246, a);    // t := 1 + x^2 / a
        var n1: int = n - 1;
        a = mulDivRound(t, n, n1) + One;
        n = n1;
    }
    // x / (1 + x^2 / a) = x / (1 + d) = x - x * d / (1 + d) = x - x * x^2/(a+x^2) for d = x^2 / a
    return x - mulDivRound(x, x2, a + x2 ~/ 256) ~/ 256;
}

/// number of terms n should be chosen as for atanh_f261()
/// fixed261 atan(fixed261 x);
@pure
@inline
fun atan_f261_inlined(x: int, n: int): int {
    var x2: int = mulrshiftr256(x, x);  // x^2 as fixed266
    var One: int = (1 << 254);
    var a: int = One ~/ n + (1 << 255);  // a := 2 + 1/n as fixed254
    repeat (n - 1) {
        // a := 1 + (1 + x^2 / a)(1 + 1/n) as fixed254
        var t: int = One + mulDivRound(x2, 1 << 242, a);    // t := 1 + x^2 / a
        var n1: int = n - 1;
        a = mulDivRound(t, n, n1) + One;
        n = n1;
    }
    // x / (1 + x^2 / a) = x / (1 + d) = x - x * d / (1 + d) = x - x * x^2/(a+x^2) for d = x^2 / a
    return x - mulDivRound(x, x2, a + x2 ~/ 4096) ~/ 4096;
}

/// fixed261 atan(fixed261 x);
@pure
@inline_ref
fun atan_f261(x: int, n: int): int {
    return atan_f261_inlined(x, n);
}

/// computes (q,a,b) such that q is approximately atan(x)/atan(1/32) and a+b*I=(1+I/32)^q as fixed255
/// then b/a=atan(q*atan(1/32)) exactly, and (a,b) is almost a unit vector pointing in the direction of (1,x)
/// must have |x|<1.1, x is fixed24
/// (int, fixed255, fixed255) atan_aux_prereduce(fixed24 x);
@pure
@inline_ref
fun atan_aux_prereduce(x: int): (int, int, int) {
    var xu: int = abs(x);
    var tc: int = 7214596;  // tan(13*theta) as fixed24 where theta=atan(1/32)
    var t1: int = mulDivRound(xu - tc, 1 << 88, xu * tc + (1 << 48));   // tan(x') as fixed64 where x'=atan(x)-13*theta
    // t1/(3+t1^2) * 3073/32 = x'/3 * 3072/32 = x' / (96/3072) = x' / theta
    var q: int = mulDivRound(t1 * 3073, 1 << 59, t1 * t1 + (3 << 128)) + 13;   // approximately round(atan(x)/theta), 0<=q<=25
    var (pa, pb) = (33226912, 5232641);  // (32+I)^5
    var (qh, ql) = divMod(q, 5);
    var (a, b) = (1 << (5 * (51 - q)), 0);    // (1/32^q, 0) as fixed255
    repeat (ql) {
        // a+b*I *= 32+I
        b.stackMoveToTop();
        (a, b) = (sub_rev(b, 32 * a), a + 32 * b);   // same as (32 * a - b, 32 * b + a), but more efficient
    }
    repeat (qh) {
        // a+b*I *= (32+I)^5 = pa + pb*I
        (a, b) = (a * pa - b * pb, a * pb + b * pa);
    }
    var xs: int = sign(x);
    return (xs * q, a, xs * b);
}

/// compute (q, z) such that atan(x)=q*atan(1/32)+z for -1 <= x < 1
/// this function is reasonably accurate (error < 7 ulp with ulp = 2^-261), but it consumes >7k gas
/// this is sufficient for most purposes
/// (int, fixed261) atan_aux(fixed256 x)
@pure
@inline_ref
fun atan_aux_f256(x: int): (int, int) {
    var (q, a, b) = atan_aux_prereduce(x ~>> 232);   // convert x to fixed24
    // now b/a = tan(q*atan(1/32)) exactly, where q is near atan(x)/atan(1/32); so b/a is near x
    // compute y = u/v = (a*x-b)/(a+b*x) as fixed261 ; then |y|<0.0167 = 1.07/64 and atan(x)=atan(y)+q*atan(1/32)
    var (u, ul) = mulrshiftr256mod(a, x);
    u = (ul ~>> 250) + ((u - b) << 6);  // |u| < 1/32, convert fixed255 -> fixed261
    var v: int = a + mulrshiftr256(b, x);    // v is scalar product of (a,b) and (1,x), it is approximately in [1..sqrt(2)] as fixed255
    var y: int = mulDivRound(u, 1 << 255, v);    // y = u/v as fixed261
    var z: int = atan_f261_inlined(y, 18);   // z = atan(x)-q*atan(1/32)
    return (q, z);
}

/// compute (q, z) such that atan(x)=q*atan(1/32)+z for -1 <= x < 1
/// this function is very accurate (error < 2 ulp), but it consumes >7k gas
/// in most cases, faster function atan_aux_f256() should be used
/// (int, fixed261) atan_auxx(fixed256 x)
@pure
@inline_ref
fun atan_auxx_f256(x: int): (int, int) {
    var (q, a, b) = atan_aux_prereduce(x ~>> 232);   // convert x to fixed24
    // now b/a = tan(q*atan(1/32)) exactly, where q is near atan(x)/atan(1/32); so b/a is near x
    // compute y = (a*x-b)/(a+b*x) as fixed261 ; then |y|<0.0167 = 1.07/64 and atan(x)=atan(y)+q*atan(1/32)
    // use sort of double precision arithmetic for this
    var (u, ul) = mulrshiftr256mod(a, x);
    ul /= 2;
    u -= b;    // |u| < 1/32 as fixed255
    var (v, vl) = mulrshiftr256mod(b, x);
    vl /= 2;
    v += a;    // v is scalar product of (a,b) and (1,x), it is approximately in [1..sqrt(2)] as fixed255
    // y = (u + ul*eps) / (v + vl*eps) = u/v + (ul - vl * u/v)/v * eps where eps=1/2^255
    var (y, r) = lshift255divmodr(u, v);    // y = u/v as fixed255
    var yl: int = mulDivRound(ul + r, 1 << 255, v) - mulDivRound(vl, y, v);   // y/2^255 + yl/2^510 represent u/v
    y = (yl ~>> 249) + (y << 6);   // convert y to fixed261
    var z: int = atan_f261_inlined(y, 18);   // z = atan(x)-q*atan(1/32)
    return (q, z);
}

/// consumes ~ 8k gas
/// fixed255 atan(fixed255 x);
@pure
@inline_ref
fun atan_f255(x: int): int {
    var s: int = (x ~>> 256);
    x.stackMoveToTop();
    if (s) {
        x = lshift256divr(-1 << 255, x);   // x:=-1/x as fixed256
    } else {
        x *= 2;   // convert to fixed256
    }
    var (q, z) = atan_aux_f256(x);
    // now atan(x) = z + q*atan(1/32) + s*(Pi/2), z is fixed261
    var (Pi_h, Pi_l) = Pi_xconst_f254();   // Pi/2 as fixed255 + fixed383
    var (qh, ql) = mulrshiftr6mod(q, Atan1_32_f261());
    return qh + s * Pi_h + (z + ql + mulDivRound(s, Pi_l, 1 << 122)) ~/ 64;
}

/// computes atan(x) for -1 <= x < 1 only
/// fixed256 atan_small(fixed256 x);
@pure
@inline_ref
fun atan_f256_small(x: int): int {
    var (q, z) = atan_aux_f256(x);
    // now atan(x) = z + q*atan(1/32), z is fixed261
    var (qh, ql) = mulrshiftr5mod(q, Atan1_32_f261());
    return qh + (z + ql) ~/ 32;
}

/// fixed255 asin(fixed255 x);
@pure
@inline_ref
fun asin_f255(x: int): int {
    var a: int = fixed255_One - fixed255_sqr(x);  // a:=1-x^2
    if (!a) {
    return sign(x) * Pi_const_f254();   // Pi/2 or -Pi/2
    }
    var y: int = fixed255_sqrt(a);   // sqrt(1-x^2)
    var t: int = -lshift256divr(x, (-1 << 255) - y);   // t = x/(1+sqrt(1-x^2)) avoiding overflow
    return atan_f256_small(t);   // asin(x)=2*atan(t)
}

/// fixed254 acos(fixed255 x);
@pure
@inline_ref
fun acos_f255(x: int): int {
    var Pi: int = Pi_const_f254();
    if (x == (-1 << 255)) {
        return Pi;   // acos(-1) = Pi
    }
    Pi /= 2;
    var y: int = fixed255_sqrt(fixed255_One - fixed255_sqr(x));   // sqrt(1-x^2)
    var t: int = lshift256divr(x, (-1 << 255) - y);   // t = -x/(1+sqrt(1-x^2)) avoiding overflow
    return Pi + atan_f256_small(t) ~/ 2;   // acos(x)=Pi/2 + 2*atan(t)
}

/// consumes ~ 10k gas
/// fixed248 asin(fixed248 x)
@pure
@inline
fun fixed248_asin(x: int): int {
    return asin_f255(x << 7) ~>> 7;
}

/// consumes ~ 10k gas
/// fixed248 acos(fixed248 x)
@pure
@inline
fun fixed248_acos(x: int): int {
    return acos_f255(x << 7) ~>> 6;
}

/// consumes ~ 7500 gas
/// fixed248 atan(fixed248 x);
@pure
@inline_ref
fun fixed248_atan(x: int): int {
    var s: int = (x ~>> 249);
    x.stackMoveToTop();
    if (s) {
        s = sign(s);
        x = lshift256divr(-1 << 248, x);   // x:=-1/x as fixed256
    } else {
        x <<= 8;   // convert to fixed256
    }
    var (q, z) = atan_aux_f256(x);
    // now atan(x) = z + q*atan(1/32) + s*(Pi/2), z is fixed261
    return (z ~/ 64 + s * Pi_const_f254() + mulDivRound(q, Atan1_32_f261(), 64)) ~/ 128;   // compute in fixed255, then convert
}

/// fixed248 acot(fixed248 x);
@pure
@inline_ref
fun fixed248_acot(x: int): int {
    var s: int = (x ~>> 249);
    x.stackMoveToTop();
    if (s) {
        x = lshift256divr(-1 << 248, x);   // x:=-1/x as fixed256
        s = 0;
    } else {
        x <<= 8;   // convert to fixed256
        s = sign(x);
    }
    var (q, z) = atan_aux_f256(x);
    // now acot(x) = - z - q*atan(1/32) + s*(Pi/2), z is fixed261
    return (s * Pi_const_f254() - z ~/ 64 - mulDivRound(q, Atan1_32_f261(), 64)) ~/ 128;   // compute in fixed255, then convert
}

/*-------------------- PSEUDO-RANDOM NUMBERS ------------------*/

/// random number with standard normal distribution N(0,1)
/// generated by Kinderman--Monahan ratio method modified by J.Leva
/// spends ~ 2k..3k gas on average
/// fixed252 nrand();
@inline_ref
fun nrand_f252(): int {
    var (x, s, t, A, B, r0) = (nan(), 29483 << 236, -3167 << 239, 12845, 16693, 9043);
    // 4/sqrt(e*Pi) = 1.369 loop iterations on average
    do {
        var (u, v) = (random.uint256() / 16 + 1, mulDivRound(random.uint256() - (1 << 255), 7027, 1 << 16));   // fixed252; 7027=ceil(sqrt(8/e)*2^12)
        var va: int = abs(v);
        var (u1, v1) = (u - s, va - t);   // (u - 29483/2^16, abs(v) + 3167/2^13) as fixed252
        // Q := u1^2 + v1 * (A*v1 - B*u1) as fixed252 where A=12845/2^16, B=16693/2^16
        var Q: int = mulDivRound(u1, u1, 1 << 252) + mulDivRound(v1, mulDivRound(v1, A, 1 << 16) - mulDivRound(u1, B, 1 << 16), 1 << 252);
        // must have 9043 / 2^15 < Q < 9125 / 2^15, otherwise accept if smaller, reject if larger
        var Qd: int = (Q >> 237) - r0;
        if ((Qd < 9125 - 9043) & (va / u < 16)) {
            x = mulDivRound(v, 1 << 252, u);    // x:=v/u as fixed252; reject immediately if |v/u| >= 16
            if (Qd >= 0) {
                // immediately accept if Qd < 0
                // rarely taken branch - 0.012 times per call on average
                // check condition v^2 < -4*u^2*log(u), or equivalent condition u < exp(-x^2/4) for x=v/u
                var xx: int = mulrshiftr256(x, x) ~/ 4;   // x^2/4 as fixed248
                var ex: int = fixed248_exp(-xx) * 16;   // exp(-x^2/4) as fixed252
                if (u > ex) {
                    x = nan();      // condition false, reject
                }
            }
        }
    } while (!(~ is_nan(x)));
    return x;
}

/// generates a random number approximately distributed according to the standard normal distribution
/// much faster than nrand_f252(), should be suitable for most purposes when only several random numbers are needed
/// fixed252 nrand_fast();
@inline_ref
fun nrand_fast_f252(): int {
    var t: int = -3 << 253;   // -6. as fixed252
    repeat (12) {
        t += random.uint256() / 16;         // add together 12 uniformly random numbers
    }
    return t;
}

/// random number uniformly distributed in [0..1)
/// fixed248 random();
@inline
fun fixed248_random(): int {
    return random.uint256() >> 8;
}

/// random number with standard normal distribution
/// fixed248 nrand();
@inline
fun fixed248_nrand(): int {
    return nrand_f252() ~>> 4;
}

/// generates a random number approximately distributed according to the standard normal distribution
/// fixed248 nrand_fast();
@inline
fun fixed248_nrand_fast(): int {
    return nrand_fast_f252() ~>> 4;
}

@pure
fun tuple.tset<X>(mutate self, idx: int, value: X): void
    asm(self value idx) "SETINDEXVAR";

// computes 1-acos(x)/Pi by a very simple, extremely slow (~70k gas) and imprecise method
// fixed256 acos_prepare_slow(fixed255 x);
@inline
fun acos_prepare_slow_f255(x: int): int {
  x -= (x == 0) as int;
  var t: int = 1;
  repeat (255) {
    t = t * sign(x) * 2 + 1;    // decode Gray code (sign(x_0), sign(x_1), ...)
    x = (-1 << 255) - mulDivRound(x, - x, 1 << 254);    // iterate x := 2*x^2 - 1 = cos(2*acos(x))
  }
  return abs(t);
}

// extremely slow (~70k gas) and somewhat imprecise (very imprecise when x is small), for testing only
// fixed254 acos_slow(fixed255 x);
@inline_ref
fun acos_slow_f255(x: int): int {
  var t: int = acos_prepare_slow_f255(x);
  return - mulrshiftr256(t + (-1<<256), Pi_const_f254());
}

// fixed255 asin_slow(fixed255 x);
@inline_ref
fun asin_slow_f255(x: int): int {
  var t: int = acos_prepare_slow_f255(abs(x)) % (1 << 255);
  return mulDivRound(t, Pi_const_f254(), 1 << 255) * sign(x);
}

@inline_ref
fun test_nrand(n: int): tuple {
  var t: tuple = createEmptyTuple();
  repeat (255) {
    t.push(0);
  }
  repeat (n) {
    var x: int = fixed248_nrand();
    var bucket: int = (abs(x) >> 243);   // 255 buckets starting from x=0, each 1/32 wide
    var at_bucket: int = t.get(bucket);
    t.tset(bucket, at_bucket + 1);
  }
  return t;
}

@method_id(10000)
fun geom_mean_test(x: int, y: int): int {
    return geom_mean(x, y);
}
@method_id(10001)
fun tan_f260_test(x: int): int {
    return tan_f260(x);
}
@method_id(10002)
fun sincosm1_f259_test(x: int): (int, int) {
    return sincosm1_f259(x);
}
@method_id(10003)
fun sincosn_f256_test(x: int, y: int): (int, int) {
    return sincosn_f256(x, y);
}
@method_id(10004)
fun sincosm1_f256_test(x: int): (int, int) {
    return sincosm1_f256(x);
}
@method_id(10005)
fun tan_aux_f256_test(x: int): (int, int) {
    return tan_aux_f256(x);
}
@method_id(10006)
fun fixed248_tan_test(x: int): int {
    return fixed248_tan(x);
}
/*
  (int) atanh_alt_f258_test(x) method_id(10007) {
      return atanh_alt_f258(x);
  }
*/
@method_id(10008)
fun atanh_f258_test(x:int, y:int): int {
    return atanh_f258(x, y);
}
@method_id(10009)
fun atanh_f261_test(x:int, y:int): int {
    return atanh_f261(x, y);
}

@method_id(10010)
fun log2_aux_f256_test(x:int): (int, int) {
    return log2_aux_f256(x);
}
@method_id(10011)
fun log_aux_f256_test(x:int): (int, int) {
    return log_aux_f256(x);
}
@method_id(10012)
fun fixed248_pow_test(x:int, y:int): int {
    return fixed248_pow(x, y);
}
@method_id(10013)
fun exp_log_div(x:int, y:int): int {
    return fixed248_exp(fixed248_log(x << 248) ~/ y);
}
@method_id(10014)
fun fixed248_log_test(x:int): int {
    return fixed248_log(x);
}
@method_id(10015)
fun log_aux_f257_test(x:int): (int,int) {
    return log_aux_f257(x);
}
@method_id(10016)
fun fixed248_sincos_test(x:int): (int,int) {
    return fixed248_sincos(x);
}
@method_id(10017)
fun fixed248_exp_test(x:int): int {
    return fixed248_exp(x);
}
@method_id(10018)
fun fixed248_exp2_test(x:int): int {
    return fixed248_exp2(x);
}
@method_id(10019)
fun expm1_f257_test(x:int): int {
    return expm1_f257(x);
}
@method_id(10020)
fun atan_f255_test(x:int): int {
    return atan_f255(x);
}
@method_id(10021)
fun atan_f259_test(x:int, n:int): int {
    return atan_f259(x, n);
}
@method_id(10022)
fun atan_aux_f256_test(x:int): (int, int) {
    return atan_aux_f256(x);
}
@method_id(10023)
fun asin_f255_test(x:int): int {
    return asin_f255(x);
}
@method_id(10024)
fun asin_slow_f255_test(x:int): int {
    return asin_slow_f255(x);
}
@method_id(10025)
fun acos_f255_test(x:int): int {
    return acos_f255(x);
}
@method_id(10026)
fun acos_slow_f255_test(x:int): int {
    return acos_slow_f255(x);
}
@method_id(10027)
fun fixed248_atan_test(x:int): int {
    return  fixed248_atan(x);
}
@method_id(10028)
fun fixed248_acot_test(x:int): int {
    return  fixed248_acot(x);
}

fun main() {
  var One: int = 1;
  // repeat(76 / 4) { One *= 10000; }
  var sqrt2: int = geom_mean(One, 2 * One);
  var sqrt3: int = geom_mean(One, 3 * One);
	  // return geom_mean(-1 - (-1 << 256), -1 - (-1 << 256));
	  // return geom_mean(-1 - (-1 << 256), -2 - (-1 << 256));
	  // return geom_mean(-1 - (-1 << 256), 1 << 255);
	  // return (sqrt2, geom_mean(sqrt2, One));  // (sqrt(2), 2^(1/4))
	  // return (sqrt3, geom_mean(sqrt3, One));  // (sqrt(3), 3^(1/4))
	  // return geom_mean(3 << 254, 1 << 254);
	  // return geom_mean(3, 5);
	  // return tan_f260(115641670674223639132965820642403718536242645001775371762318060545014644837101 - 1);
	  // return tan_f260(15 << 252);  // tan(15/256) * 2^260
	  // return sincosm1_f259(1 << 255);  // (sin,1-cos)(1/16) * 2^259
	  // return sincosm1_f259(115641670674223639132965820642403718536242645001775371762318060545014644837101 - 1);
	  // return sincosm1_f256((1 << 255) - 1 + (1 << 255));  // (sin,1-cos)(1-2^(-256))
	  // return sincosm1_f256(Pi_const_f254());  // (sin,1-cos)(Pi/4)
	  // return sincosn_f256(Pi_const_f254(), 0);  // (sin,-cos)(Pi/4)
	  // return sincosn_f256((1 << 255) + 1, 0);  // (sin,-cos)(1/2+1/2^256)
	  // return sincosn_f256(1 << 254, 0);
	  // return sincosn_f256(stackMoveToTop(15) << 252, 0);  // (sin,-cos)(15/16)
	  // return sincosm1_f256(stackMoveToTop(15) << 252);  // (sin,1-cos)(15/16)
	  // return sincosn_f256(60628596148627720713372490462954977108898896221398738326462025186323149077698, 0);  // (sin,-cos)(Pi/6)
	  // return sincosm1_f256(60628596148627720713372490462954977108898896221398738326462025186323149077698);  // (sin,1-cos)(Pi/6)
	  // return tan_aux_f256(1899 << 245);  // (p,q) such that p/q=tan(1899/2048)
	  // return fixed248_tan(11 << 248);  // tan(11)
	  // return atanh_alt_f258(1 << 252);  // atanh(1/64) * 2^258
    // return atanh_f258(1 << 252, 18);  // atanh(1/64) * 2^258
    // return atanh_f261(mulDivRound(64, 1 << 255, 55), 18);   // atanh(1/55) * 2^261
    // return log2_aux_f256(1 << 255);
    // return log2_aux_f256(-1 - (-1 << 256));   // log2(2-1/2^255))*2^256 ~ 2^256 - 1.43
    // return log_aux_f256(-1 - (-1 << 256));
    // return log_aux_f256(3);  // log(3/2)*2^256
    // return fixed248_pow(3 << 248, 3 << 248);  // 3^3
    // return fixed248_exp(fixed248_log(5 << 248) ~/ 7);   // exp(log(5)/7) = 5^(1/7)
    // return fixed248_log(Pi_const_f254() ~>> 6);  // log(Pi)
	  // return atanh_alt_f258(1 << 255);  // atanh(1/8) * 2^258
    // return atanh_f258(1 << 255, 37);  // atanh(1/8) * 2^258
    // return atanh_f258(81877371507464127617551201542979628307507432471243237061821853600756754782485, 36);   // atanh(sqrt(2)/8) * 2^258
    // return log_aux_f257(Pi_const_f254());   // log(Pi/4)
    // return log_aux_f257(3 << 254);   // log(3)
	  // return atanh_alt_f258(81877371507464127617551201542979628307507432471243237061821853600756754782485);   // atanh(sqrt(2)/8) * 2^258
    // return fixed248_sincos(Pi_const_f254() ~/ (64 * 3));   // (sin,cos)(Pi/3)
    // return fixed248_exp(3 << 248);  // exp(3)*2^248
    // return fixed248_exp2((1 << 248) ~/ 5);  // 2^(1/5)*2^248
    // return fixed248_pow(3 << 248, -3 << 247);  // 3^(-1.5)
    // return fixed248_pow(10 << 248, -70 << 248);  // 10^(-70)
    // return fixed248_pow(fixed248_Pi_const(), stackMoveToTop(3) << 248);   // Pi^3 ~ 31.006, computed more precisely
    // return fixed248_pow(fixed248_Pi_const(), fixed248_Pi_const());   // Pi^Pi, more precisely
  // return fixed248_exp(fixed248_log(fixed248_Pi_const()) * 3);   // Pi^3 ~ 31.006
  // return fixed248_exp(mulDivRound(fixed248_log(fixed248_Pi_const()), fixed248_Pi_const(), 1 << 248));   // Pi^Pi
  // return fixed248_sin(fixed248_log(fixed248_exp(fixed248_Pi_const())));   // sin(log(e^Pi))
    // return expm1_f257(1 << 255);  // (exp(1/4)-1)*2^256
    // return expm1_f257(-1 << 256);  // (exp(-1/2)-1)*2^256 (argument out of range, will overflow)
  // return expm1_f257(log2_const_f256());  // (exp(log(2)/2)-1)*2^256
  // return expm1_f257(- log2_const_f256());  // (exp(-log(2)/2)-1)*2^256
  // return tanh_f258(log2_const_f256(), 17);  // tanh(log(2)/4)*2^258
    // return atan_f255(0xa0 << 247);
    // return atan_f259(1 << 255, 26);   // atan(1/16)
    // return atan_f259(stackMoveToTop(2273) << 244, 26);   // atan(2273/2^15)
    // return atan_aux_f256(0xa0 << 248);
    // return atan_aux_f256(-1 - (-1 << 256));
    // return atan_aux_f256(-1 << 256);
    // return atan_aux_f256(1);  // atan(1/2^256)*2^261 = 32
  //return fixed248_nrand();
  // return test_nrand(100000);
  var One2: int = 1 << 255;
    // return asin_f255(One);
    // return asin_f255(-2 * One ~/ -3);
  var arg: int = mulDivRound(12, One2, 17);  // 12/17
    // return [ asin_slow_f255(arg), asin_f255(arg) ];
    // return [ acos_slow_f255(arg), acos_f255(arg) ];
  // return 4 * atan_f255(One ~/ 5) - atan_f255(One ~/ 239);   // 4 * atan(1/5) - atan(1/239) = Pi/4 as fixed255
  var One3: int = 1 << 248;
    // return fixed248_atan(One) ~/ 5);   // atan(1/5)
    // return fixed248_acot(One ~/ 239);   // atan(1/5)
}

/**
    method_id | in | out
@testcase | 10000 |   -1-(-1<<256) -1-(-1<<256) | 115792089237316195423570985008687907853269984665640564039457584007913129639935
@testcase | 10000 |   -1-(-1<<256) -2-(-1<<256) | 115792089237316195423570985008687907853269984665640564039457584007913129639934
@testcase | 10000 |   -1-(-1<<256) 1<<255 | 81877371507464127617551201542979628307507432471243237061821853600756754782485
@testcase | 10000 |   1 2 | 1
@testcase | 10000 |   1 3 | 2
@testcase | 10000 |   3<<254 1<<254 | 50139445418395255283694704271811692336355250894665672355503583528635147053497
@testcase | 10000 |   3 5 | 4
@testcase | 10001 |   115641670674223639132965820642403718536242645001775371762318060545014644837101-1 | 115792089237316195423570985008687907853269984665640564039457584007913129639935
@testcase | 10001 |   15<<252 | 108679485937549714997960660780289583146059954551846264494610741505469565211201

@testcase | 10002 |   1<<255 | 57858359242454268843682786479537198006144860419130642837770554273561536355094 28938600351875109040123440645416448095273333920390487381363947585666516031269
@testcase | 10002 |   90942894222941581070058735694432465663348344332098107489693037779484723616546 | 90796875678616203090520439851979829600860326752181983760731669850687818036503 71369031536005973567205947792557760023823761636922618688720973932041901854510
@testcase | 10002 | 115641670674223639132965820642403718536242645001775371762318060545014644837100 | 115341536360906404779899502576747487978354537254490211650198994186870666100480 115341536360906404779899502576747487978354537254490211650198994186870666100479
@testcase | 10003 |   90942894222941581070058735694432465663348344332098107489693037779484723616546 0 | 81877371507464127617551201542979628307507432471243237061821853600756754782485 -81877371507464127617551201542979628307507432471243237061821853600756754782486
@testcase | 10003 |   (1<<255)+1 0 | 55513684748706254392157395574451324146997108788015526773113170656738693667657 -101617118319522600545601981648807607350213579319835970884288805016705398675944
@testcase | 10003 |   1<<254 0 | 28647421327665059059430596260119789787021370826354543144805343654507971817712 -112192393597863122712065585177748900737784171216163716639418346853706594800924
@testcase | 10003 |   15<<252 0 | 93337815620236900315136494926097782162348358704087992554326802765553037216157 -68526346066204767396483080633934170508153877799043171682610011603005473885083
@testcase | 10004 |   15<<252 | 93337815620236900315136494926097782162348358704087992554326802765553037216158 94531486342222856054175808749507474690232213733194784713695144809815311509707
@testcase | 10003 |   60628596148627720713372490462954977108898896221398738326462025186323149077698 0 | 57896044618658097711785492504343953926634992332820282019728792003956564819968 -100278890836790510567389408543623384672710501789331344711007167057270294106993
@testcase | 10004 |   60628596148627720713372490462954977108898896221398738326462025186323149077698 | 57896044618658097711785492504343953926634992332820282019728792003956564819968 31026396801051369712363152930129046361118965752618438656900833901285671065886
@testcase | 10005 |   1899<<245 | -115784979074977116522606932816046735344768048129666123117516779696532375620701 -86847621900007587791673148476644866514014227467564880140262768165345715058771
@testcase | 10006 |   11<<248 | -102200470999497240398685962406597118965525125432278008915850368651878945159221
@testcase | 10008 |   1<<252 18 | 7237594612640731814076778712183932891481921212865048737772958953246047977071
@testcase! | 10009 |   64*(1<<255)//55 18 | 67377367986958444187782963285047188951340314639925508148698906136973510008513
@testcase | 10010 |   1<<255 | 0 255
@testcase | 10011 |   -1-(-1<<256) | 80260960185991308862233904206310070533990667611589946606122867505419956976171 255
@testcase | 10012 |   3<<248 3<<248 | 12212446911748192486079752325135052781399568695204278238536542063334587891712
@testcase | 10013 |   5 7 | 569235245303856216139605450142923208167703167128528666640203654338408315932
@testcase | 10014 |   1420982722233462204219667745225507275989817880189032929526453715304448806508 | 517776035526939558040896860590142614178014859368681705591403663865964112176
@testcase | 10008 |   1<<255 37 | 58200445412255555045265806996802932280233368707362818578692888102488340124094
@testcase | 10008 |   81877371507464127617551201542979628307507432471243237061821853600756754782485 36 | 82746618329032515754939514227666784789465120373484337368014239356561508382845
@testcase | 10015 |   90942894222941581070058735694432465663348344332098107489693037779484723616546 | -55942510554172181731996424203087263676819062449594753161692794122306202470292 256
@testcase | 10015 |   3<<254 | -66622616410625360568738677407433830899150908037353507097280251369610028875158 256
@testcase | 10016 |   90942894222941581070058735694432465663348344332098107489693037779484723616546//(64*3) | 391714417331212931903864877123528846377775397614575565277371746317462086355 226156424291633194186662080095093570025917938800079226639565593765455331328
@testcase | 10017 |   3<<248 | 9084946421051389814103830025729847734065792062362132089390904679466687950835
@testcase | 10018 |   (1<<248)//5 | 519571025111621076330285524602776985448579272766894385941850747946908706857
@testcase | 10012 |   3<<248 -3<<247 | 87047648295825095978636639360784188083950088358794570061638165848324908079
@testcase | 10012 |   10<<248 -70<<248 | 45231
@testcase | 10012 |   1420982722233462204219667745225507275989817880189032929526453715304448806508 3<<248 | 14024537329227316173680050897643053638073167245065581681188087336877135047241
@testcase | 10012 |   1420982722233462204219667745225507275989817880189032929526453715304448806508 1420982722233462204219667745225507275989817880189032929526453715304448806508 | 16492303277433924047657446877966346821161732581471802839855102123372676002295
@testcase | 10019 |   1<<255 | 65775792789545756849501669218806308540691279864498696756901136302101823231959
@testcase | 10019 |   -1<<255 | -51226238931640701466578648374135745377468902266335737558089915608594425303282

@testcase | 10020 |   160<<247 | 32340690885082755723307749066376646841771751777398167772823878380310576779097
@testcase | 10021 |   1<<255 26 | 57820835337111819566482910321201859268121322500887685881159030272507322418551
@testcase | 10021 |   2273<<244 26 | 64153929153128256059565403901040178355488584937372975321150754259394300105908
@testcase | 10022 |   160<<248 | 18 -13775317617017974742132028403521581424991093186766868001115299479309514610238
@testcase | 10022 |   -1-(-1<<256) | 25 16312150880916231694896252427912541090503675654570543195394548083530005073282
@testcase | 10022 |   -1<<256 | -25 -16312150880916231694896252427912541090503675654570543195394548083530005073298
@testcase | 10022 |   1 | 0 32

@testcase | 10023 |   1<<255 | 90942894222941581070058735694432465663348344332098107489693037779484723616546
@testcase | 10023 |   (1-(1<<255))//-3 | 19675212872822715586637341573564384553677006914302429002469838095945333339604
@testcase | 10023 |   12*(1<<255)//17 | 45371280744427205854111943101074857545572584208710061167826656461897302968384
@testcase | 10024 |   12*(1<<255)//17 | 45371280744427205854111943101074857545572584208710061167826656461897302968387
@testcase | 10025 |   12*(1<<255)//17 | 22785806739257187607973396296678804058887880061694023160933190658793710324081
@testcase | 10026 |   12*(1<<255)//17 | 22785806739257187607973396296678804058887880061694023160933190658793710324080

@testcase | 10027 |   (1<<248)//5 | 89284547973388213553327350968415123522888028497458323165947767504203347189
@testcase | 10028 |   (1<<248)//239 | 708598849781543798951441405045469962900811296151941404481049216461523216127
*/
