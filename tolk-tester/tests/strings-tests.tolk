get ascii_slice(): slice {
  return"string";
}

get raw_slice(): slice {
  return stringHexToSlice("abcdef");
}

get addr_slice(): address {
  return address("Ef8zMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzM0vF");
}

get string_hex(): int {
  return stringToBase256("ABCDEFGHIJKLMNOPQRSTUVWXYZ012345");
}

get fun string_minihash(): int {    // 'get' and 'get fun' both possible
  return stringSha256_32("transfer(slice, int)");
}

get fun string_maxihash(): int {
  return stringSha256("transfer(slice, int)");
}

get fun string_crc32(): int {
  return stringCrc32("transfer(slice, int)");
}

get fun string_crc16(): int {
  return stringCrc16("transfer(slice, int)");
}

@pure
fun newc(): builder
asm "NEWC";
fun builder.endcs(self): slice
asm "ENDC" "CTOS";
@pure
fun sdeq(s1: slice, s2: slice): int
asm "SDEQ";

fun calcSliceBytes(slice: slice): tuple {
  var t = createEmptyTuple();
  var n = 0;
  while (n = slice.remainingBitsCount()) {
    t.push(slice.loadUint(min(8, n)));
  }
  return t;
}

fun main() {
  var s_ascii: slice = ascii_slice();
  var s_raw: slice = raw_slice();
  var s_addr: address = addr_slice();
  var i_hex: int = string_hex();
  var i_mini: int = string_minihash();
  var i_maxi: int = string_maxihash();
  var i_crc32: int = string_crc32();
  var i_crc16: int = string_crc16();
  assert(sdeq(s_ascii, newc().storeUint(0x737472696E67, 12 * 4).endcs())) throw 101;
  assert(sdeq(s_raw, newc().storeUint(0xABCDEF, 6 * 4).endcs())) throw 102;
  assert(sdeq(s_addr as slice, newc().storeUint(4, 3).storeInt(-1, 8)
    .storeUint(0x3333333333333333333333333333333333333333333333333333333333333333, 256).endcs()), 103);
  assert(i_hex == 0x4142434445464748494A4B4C4D4E4F505152535455565758595A303132333435) throw 104;
  assert(i_mini == 0x7a62e8a8) throw 105;
  assert(i_maxi == 0x7a62e8a8ebac41bd6de16c65e7be363bc2d2cbc6a0873778dead4795c13db979) throw 106;
  assert(i_crc32 == 2235694568 && i_crc32 == 0x8541fde8) throw 107;
  assert(i_crc16 == 52550 && i_crc16 == 0xCD46) throw 108;
  return 0;
}

@method_id(101)
fun test1() {
  return calcSliceBytes("ABCD");
}

/**
@testcase | 0   | | 0
@testcase | 101 | | [ 65 66 67 68 ]

@code_hash 58447050269190289721084012139099481162782788646785441106022886746601529758643
*/
