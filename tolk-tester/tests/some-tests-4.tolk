const __precisionZeros = 1000000000000000000; // 18 zeros
const __base0          = 1000000043929018416; // 4x every year (3600 * (24 * 365 + 6) seconds)

const __base1 = __base0  * __base0  / __precisionZeros;
const __base2 = __base1  * __base1  / __precisionZeros;
const __base3 = __base2  * __base2  / __precisionZeros;
const __base4 = __base3  * __base3  / __precisionZeros;
const __base5 = __base4  * __base4  / __precisionZeros;
const __base6 = __base5  * __base5  / __precisionZeros;
const __base7 = __base6  * __base6  / __precisionZeros;
const __base8 = __base7  * __base7  / __precisionZeros;
const __base9 = __base8  * __base8  / __precisionZeros;
const __base10 = __base9  * __base9  / __precisionZeros;
const __base11 = __base10 * __base10 / __precisionZeros;
const __base12 = __base11 * __base11 / __precisionZeros;
const __base13 = __base12 * __base12 / __precisionZeros;
const __base14 = __base13 * __base13 / __precisionZeros;
const __base15 = __base14 * __base14 / __precisionZeros;
const __base16 = __base15 * __base15 / __precisionZeros;
const __base17 = __base16 * __base16 / __precisionZeros;
const __base18 = __base17 * __base17 / __precisionZeros;
const __base19 = __base18 * __base18 / __precisionZeros;
const __base20 = __base19 * __base19 / __precisionZeros;
const __base21 = __base20 * __base20 / __precisionZeros;
const __base22 = __base21 * __base21 / __precisionZeros;
const __base23 = __base22 * __base22 / __precisionZeros;
const __base24 = __base23 * __base23 / __precisionZeros;
const __base25 = __base24 * __base24 / __precisionZeros;
const __base26 = __base25 * __base25 / __precisionZeros;
const __base27 = __base26 * __base26 / __precisionZeros;
const __base28 = __base27 * __base27 / __precisionZeros;
const __base29 = __base28 * __base28 / __precisionZeros;
const __base30 = __base29 * __base29 / __precisionZeros;

const ERROR_EXP_TOO_LARGE = 10970;

@method_id(101)
fun test1(value: int, exp: int): int {
    assert (exp <= 0x7fffffff) throw ERROR_EXP_TOO_LARGE;

    if ((exp & 0x00000001) != 0) { value = mulDivFloor(value, __base0,  __precisionZeros); }
    if ((exp & 0x00000002) != 0) { value = mulDivFloor(value, __base1,  __precisionZeros); }
    if ((exp & 0x00000004) != 0) { value = mulDivFloor(value, __base2,  __precisionZeros); }
    if ((exp & 0x00000008) != 0) { value = mulDivFloor(value, __base3,  __precisionZeros); }

    if ((exp & 0x00000010) != 0) { value = mulDivFloor(value, __base4,  __precisionZeros); }
    if ((exp & 0x00000020) != 0) { value = mulDivFloor(value, __base5,  __precisionZeros); }
    if ((exp & 0x00000040) != 0) { value = mulDivFloor(value, __base6,  __precisionZeros); }
    if ((exp & 0x00000080) != 0) { value = mulDivFloor(value, __base7,  __precisionZeros); }

    if ((exp & 0x00000100) != 0) { value = mulDivFloor(value, __base8,  __precisionZeros); }
    if ((exp & 0x00000200) != 0) { value = mulDivFloor(value, __base9,  __precisionZeros); }
    if ((exp & 0x00000400) != 0) { value = mulDivFloor(value, __base10, __precisionZeros); }
    if ((exp & 0x00000800) != 0) { value = mulDivFloor(value, __base11, __precisionZeros); }

    if ((exp & 0x00001000) != 0) { value = mulDivFloor(value, __base12, __precisionZeros); }
    if ((exp & 0x00002000) != 0) { value = mulDivFloor(value, __base13, __precisionZeros); }
    if ((exp & 0x00004000) != 0) { value = mulDivFloor(value, __base14, __precisionZeros); }
    if ((exp & 0x00008000) != 0) { value = mulDivFloor(value, __base15, __precisionZeros); }

    if ((exp & 0x00010000) != 0) { value = mulDivFloor(value, __base16, __precisionZeros); }
    if ((exp & 0x00020000) != 0) { value = mulDivFloor(value, __base17, __precisionZeros); }
    if ((exp & 0x00040000) != 0) { value = mulDivFloor(value, __base18, __precisionZeros); }
    if ((exp & 0x00080000) != 0) { value = mulDivFloor(value, __base19, __precisionZeros); }

    if ((exp & 0x00100000) != 0) { value = mulDivFloor(value, __base20, __precisionZeros); }
    if ((exp & 0x00200000) != 0) { value = mulDivFloor(value, __base21, __precisionZeros); }
    if ((exp & 0x00400000) != 0) { value = mulDivFloor(value, __base22, __precisionZeros); }
    if ((exp & 0x00800000) != 0) { value = mulDivFloor(value, __base23, __precisionZeros); }

    if ((exp & 0x01000000) != 0) { value = mulDivFloor(value, __base24, __precisionZeros); }
    if ((exp & 0x02000000) != 0) { value = mulDivFloor(value, __base25, __precisionZeros); }
    if ((exp & 0x04000000) != 0) { value = mulDivFloor(value, __base26, __precisionZeros); }
    if ((exp & 0x08000000) != 0) { value = mulDivFloor(value, __base27, __precisionZeros); }

    if ((exp & 0x10000000) != 0) { value = mulDivFloor(value, __base28, __precisionZeros); }
    if ((exp & 0x20000000) != 0) { value = mulDivFloor(value, __base29, __precisionZeros); }
    if ((exp & 0x40000000) != 0) { value = mulDivFloor(value, __base30, __precisionZeros); }

    return value;
}

@method_id(102)
fun test2(g1: bool) {
    if (g1) {
        return __base30;
    }
    return __base30;
}

fun main() {
    return 0x7fffffff;
}

/**
@testcase | 101 | 12345678901234567890 0          | 12345678901234567890
@testcase | 101 | 12345678901234567890 2147483647 | 1152091301147391687695287037940761486473052236056853112522947
@testcase | 102 | 0  | 305482242253527345283975278648594775678
@testcase | 102 | -1 | 305482242253527345283975278648594775678

@fif_codegen
"""
  test2() PROC:<{
    IFJMP:<{ 
      305482242253527345283975278648594775678 PUSHINT   //  '1=305482242253527345283975278648594775678
    }> 
    305482242253527345283975278648594775678 PUSHINT     //  '2=305482242253527345283975278648594775678
  }>
"""
 */
