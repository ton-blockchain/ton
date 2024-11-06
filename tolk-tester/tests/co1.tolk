const int1 = 1;
const int2 = 2;

const int101: int = 101;
const int111: int = 111;

const int1r = int1;

const str1 = "const1";
const str2 = "aabbcc"s;

const str2r: slice = str2;

const str1int = 0x636f6e737431;
const str2int = 0xAABBCC;

const nibbles: int = 4;

fun iget1(): int { return int1; }
fun iget2(): int { return int2; }
fun iget3(): int { return int1+int2; }

fun iget1r(): int { return int1r; }

fun sget1(): slice { return str1; }
fun sget2(): slice { return str2; }
fun sget2r(): slice { return str2r; }

const int240: int = ((int1+int2)*10)<<3;

fun iget240(): int { return int240; }

@pure
fun newc(): builder
asm "NEWC";
@pure
fun endcs(b: builder): slice
asm "ENDC" "CTOS";
@pure
fun sdeq(s1: slice, s2: slice): int
asm "SDEQ";
@pure
fun stslicer(b: builder, s: slice): builder
asm "STSLICER";

fun main() {
    var i1: int = iget1();
    var i2: int = iget2();
    var i3: int = iget3();

    assert(i1 == 1) throw int101;
    assert(i2 == 2) throw 102;
    assert(i3 == 3) throw 103;

    var s1: slice = sget1();
    var s2: slice = sget2();
    var s3: slice = newc().stslicer(str1).stslicer(str2r).endcs();

    assert(sdeq(s1, newc().storeUint(str1int, 12 * nibbles).endcs())) throw int111;
    assert(sdeq(s2, newc().storeUint(str2int, 6 * nibbles).endcs())) throw 112;
    assert(sdeq(s3, newc().storeUint(0x636f6e737431AABBCC, 18 * nibbles).endcs())) throw 113;

    var i4: int = iget240();
    assert(i4 == 240) throw ((104));
    return 0;
}

/**
@testcase | 0 | | 0

@code_hash 61273295789179921867241079778489100375537711211918844448475493726205774530743
*/
