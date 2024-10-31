get ascii_slice(): slice {
  return"string";
}

get raw_slice(): slice {
  return "abcdef"s;
}

get addr_slice(): slice {
  return "Ef8zMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzM0vF"a;
}

get string_hex(): int {
  return "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345"u;
}

get fun string_minihash(): int {    // 'get' and 'get fun' both possible
  return "transfer(slice, int)"h;
}

get fun string_maxihash(): int {
  return "transfer(slice, int)"H;
}

get fun string_crc32(): int {
  return "transfer(slice, int)"c;
}

@pure
fun newc(): builder
asm "NEWC";
fun endcs(b: builder): slice
asm "ENDC" "CTOS";
@pure
fun sdeq(s1: slice, s2: slice): int
asm "SDEQ";

fun main() {
  var s_ascii: slice = ascii_slice();
  var s_raw: slice = raw_slice();
  var s_addr: slice = addr_slice();
  var i_hex: int = string_hex();
  var i_mini: int = string_minihash();
  var i_maxi: int = string_maxihash();
  var i_crc: int = string_crc32();
  assert(sdeq(s_ascii, newc().storeUint(0x737472696E67, 12 * 4).endcs())) throw 101;
  assert(sdeq(s_raw, newc().storeUint(0xABCDEF, 6 * 4).endcs())) throw 102;
  assert(sdeq(s_addr, newc().storeUint(4, 3).storeInt(-1, 8)
    .storeUint(0x3333333333333333333333333333333333333333333333333333333333333333, 256).endcs()), 103);
  assert(i_hex == 0x4142434445464748494A4B4C4D4E4F505152535455565758595A303132333435) throw 104;
  assert(i_mini == 0x7a62e8a8) throw 105;
  assert(i_maxi == 0x7a62e8a8ebac41bd6de16c65e7be363bc2d2cbc6a0873778dead4795c13db979) throw 106;
  assert(i_crc == 2235694568) throw 107;
  return 0;
}

/**
@testcase | 0 | | 0

@code_hash 13830542019509784148027107880226447201604257839069192762244575629978154217223
*/
