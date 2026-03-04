import "@stdlib/lisp-lists"
import "@stdlib/strings"
import "@stdlib/reflection"

const ABCD = "abcd" as string

@pure
fun string.equalsWhenNoSnakeData(self, other: string): bool
    asm "HASHCU"
        "SWAP"
        "HASHCU"
        "EQUAL"

get fun ascii_slice(): slice {
  return"string".beginParse();
}

get fun raw_slice(): slice {
  assert ("abcdef".hexToSlice().bitsEqual(stringHexToSlice("abcdef"))) throw 123;
  return stringHexToSlice("abcdef");
}

get fun addr_slice(): address {
  return address("Ef8zMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzM0vF");
}

get fun string_hex(): int {
  assert (stringToBase256("ABCDEFGHIJKLMNOPQRSTUVWXYZ012345") == "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345".toBase256()) throw 123;
  return stringToBase256("ABCDEFGHIJKLMNOPQRSTUVWXYZ012345");
}

const S_TRANSFER = "transfer(slice, int)"
const S_ALSO_TRANSFER = S_TRANSFER

get fun string_minihash(): int {
  assert (stringSha256_32("transfer(slice, int)") == S_TRANSFER.sha256_32()) throw 123;
  return stringSha256_32("transfer(slice, int)");
}

get fun string_maxihash(): int {
  assert (stringSha256(S_ALSO_TRANSFER) == "transfer(slice, int)".sha256()) throw 123;
  return stringSha256("transfer(slice, int)");
}

get fun string_crc32(): int {
  assert (string.crc32("abc") == "abc".crc32()) throw 123;
  assert (stringCrc32("transfer(slice, int)") == "transfer(slice, int)".crc32()) throw 123;
  return stringCrc32(S_ALSO_TRANSFER);
}

get fun string_crc16(): int {
  assert (stringCrc16(S_TRANSFER) == S_ALSO_TRANSFER.crc16()) throw 123;
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

fun string.bytesToArray(self): array<uint8> {
  var bytes = array<uint8> [];
  var snake = self.beginParse();
  repeat (snake.depth() + 1) {
    repeat (snake.remainingBitsCount() / 8) {
      bytes.push(snake.loadUint(8));
    }
    if (snake.remainingRefsCount()) {
      snake = snake.loadRef().beginParse();
    }
  }
  return bytes;
}

fun getName<T>() { return reflect.typeNameOf<T>() }

type EmptyTensor = ()

global counter: int;

struct Wrapper<T> {
  sb: T
}

fun makeWrap(): Wrapper<StringBuilder> {
  counter += 1;
  return Wrapper { sb: StringBuilder.create() };
}


fun main() {
  __expect_type("", "string");
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
  assert(getName<int8>().equalsWhenNoSnakeData("int8")) throw 109;
  assert(getName<int8>().equalsWhenNoSnakeData(reflect.typeNameOf<int8>())) throw 109;
  assert(reflect.typeNameOf<EmptyTensor>().beginParse().loadUint(88) == 83932693928210635055918962) throw 110;
  assert(int176.fromSlice(getName<(int, map<bool,bool>)>().beginParse()) == 15119865017903399349236740405018280815773870593949225) throw 111;
  assert(reflect.typeNameOfObject((176 as int176, 176)).equalsWhenNoSnakeData("(int176, int)")) throw 112;
  return 0;
}

fun makeSnake_ab_cd(): string {
  return beginCell().storeUint(0x6162, 16).storeRef(
    beginCell().storeUint(0x6364, 16).endCell()
  ).endCell() as unknown as string;
}

@method_id(101)
fun test1() {
  return "ABCD".bytesToArray();
}

@method_id(102)
fun test2() {
  var (s1, s2, s3) = ("", "a", "abcd");
  return (
    s1.calculateLength(),
    s2.calculateLength(),
    s3.calculateLength(),
    s3.beginParse().loadUint(32) == 0x61626364,
    s3.equalsWhenNoSnakeData("abcd"),
    s3.equalsWhenNoSnakeData(s3),
    s3.equalsWhenNoSnakeData(s2),
  )
}

// 127 bytes = 1016 bits, fits in one cell
const STR_127 = "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567";
// 128 bytes = 1024 bits, needs snake (one ref to 1 byte)
const STR_128 = "12345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678";

@method_id(103)
fun test3() {
  val s127 = STR_127.beginParse();
  val s128 = STR_128.beginParse();
  val t128 = s128.preloadRef().beginParse();
  return (
    s127.remainingBitsAndRefsCount(),
    s128.remainingBitsAndRefsCount(),
    t128.remainingBitsAndRefsCount(),
    STR_128.calculateLength(),
  )
}

@method_id(104)
fun test4() {
  var manualS = beginCell().storeUint(0x61626364, 32).endCell() as unknown as string;
  return manualS.equalsWhenNoSnakeData("abcd");
}

@method_id(105)
fun test5() {
  var chunkedS = makeSnake_ab_cd();
  return (chunkedS.equalsWhenNoSnakeData("abcd"), chunkedS.equalsWhenNoSnakeData(chunkedS), chunkedS.calculateLength());
}

@method_id(106)
fun test6() {
  var ab_cd = StringBuilder.create().append("ab").append("cd").build();
  return (ab_cd.calculateLength(), ab_cd.beginParse().remainingBitsAndRefsCount(), ab_cd.equalsWhenNoSnakeData(makeSnake_ab_cd()));
}

@method_id(107)
fun test7() {
  var s = StringBuilder{}.append("a").build();
  return (s.beginParse().depth(), s.equalsWhenNoSnakeData("a"));
}

@method_id(108)
fun test8(c: bool) {
  var sb = StringBuilder.create().append("a").append("b");
  if (c) { sb.append("c"); }
  var a_b_c = sb.build();
  var xy_a_b_c_ab_cd = StringBuilder{}.append("xy").append(a_b_c).append(makeSnake_ab_cd()).build();
  return (xy_a_b_c_ab_cd.calculateLength(), (xy_a_b_c_ab_cd as unknown as cell).depth());
}

@method_id(109)
fun test9() {
  return StringBuilder.create().append(
    StringBuilder.create().append("a").append("b").append(
      StringBuilder.create().append("c").append("d").build()
    ).build(),
  ).append(
    StringBuilder.create().append("e").build(),
  ).append(
    StringBuilder.create().append("f").append("g").build()
  ).append("h").build().bytesToArray();
}

@method_id(110)
fun test10() {
  var list = lisp_list<string> ["1", "2"];
  list.prependHead("0");
  var sb = StringBuilder.create();
  var ptr = list;
  while (!ptr.isEmpty()) {
    sb.append(ptr.popHead());
  }
  val s = sb.build();
  return (s.calculateLength(), s.bytesToArray());
}

@method_id(111)
fun test11() {
  return (
    "a\n\t\\\"\rb".bytesToArray(),
    """a\n"\"b""".bytesToArray(),
    "line1\nline2".calculateLength(),
    "say \"hello\"".calculateLength(),
    "привет".calculateLength(),
    "😀".calculateLength(),
    """new
"line""".calculateLength(),
    """a"\n\\"\"'\'b\"""".equalTo("a\"\n\\\"\"\''b\""),
    reflect.typeNameOf<StringBuilder>().calculateLength(),
  )
}

// test int.toDecimalString() and StringBuilder.appendInt()
@method_id(112)
fun test12() {
  return (
    0.toDecimalString().equalsWhenNoSnakeData("0"),
    1.toDecimalString().equalsWhenNoSnakeData("1"),
    42.toDecimalString().equalsWhenNoSnakeData("42"),
    (-1).toDecimalString().equalsWhenNoSnakeData("-1"),
    (-1234567890).toDecimalString().equalsWhenNoSnakeData("-1234567890"),
    (1<<100).toDecimalString().calculateLength(),
  )
}

@method_id(113)
fun test13() {
  val s = StringBuilder.create()
    .append("x=").appendInt(1 << 32)
    .append(", ")
    .append(StringBuilder{}.append("y=").appendInt(-1 << 32).build())
    .build();
  return s.calculateLength(); // x=4294967296, y=-4294967296
}

@method_id(114)
fun test14() {
  var ab = "ab";
  var cd = "cd";
  var abab = "abab";
  var cdcd = "cdcd";
  var ababab = "ababab";
  var cdcdcd = "cdcdcd";
  var ab__ab = StringBuilder{}.append(ab).append("").append(ab).build();
  var cd_cd = StringBuilder{}.append(cd).append(cd).build();
  var ab_cd = StringBuilder{}.append(ab).append(cd).build();
  var ab_ab_ab = StringBuilder{}.append(ab__ab).append(ab).build();
  var ab_ab_cd = StringBuilder{}.append(abab).append(cd).build();
  var ab_cd_cd = StringBuilder{}.append(ab).append(cdcd).build();
  var ab_cd_c_d = StringBuilder{}.append(ab).append("cd").append("c").append("d").build();
  return (
    abab.equalTo(abab),
    abab.equalTo(ab),
    ab.equalTo(abab),
    abab.equalTo(cd_cd),
    abab.equalTo(ab__ab),
    cdcd.equalTo(cdcd),
    abab.equalTo(ab_ab_cd),
    ab__ab.equalTo(ab__ab),
    ab_ab_cd.equalTo(ab__ab),
    ab_ab_ab.equalTo("ababab"),
    ab_ab_ab.equalTo(ab_ab_cd),
    ab_cd_c_d.equalTo("abcdcd"),
    ab_cd_c_d.equalTo("abcdc"),
    ab_cd_c_d.equalTo("abcdcde"),
    ab_cd_c_d.equalTo(ab_cd_cd),
    ab_cd.equalTo(ab_cd),
    ab_cd.equalTo("abcd"),
    ab_cd.equalTo(ab__ab),
    ab_cd.equalTo(ab_cd_cd),
    cdcdcd.equalTo(cdcdcd),
    cdcdcd.equalTo(ababab),
    ababab.equalTo(ab_ab_ab),
  )
}

@method_id(116)
fun test16() {
  // all lhs of `crc16` are constant expressions, therefore it works
  return (
    "abcd".crc16(),
    ABCD.crc16(),
    ("abcd" as string).crc16(),
    ("abcd", "", 2).0.crc16(),
    ([(), ABCD, ()] as [(), string, ()]).1.crc16(),
  )
}

@method_id(117)
fun test17() {
  return ("abcd".toBase256())
}

@method_id(118)
fun test18() {
  var c = beginCell().storeString("abcd").storeString("de").endCell();
  var s = c.beginParse();
  assert (s.remainingRefsCount() == 2) throw 123;
  var w = Wrapper<(string, string)>.fromSlice(s);
  var (s1, s2) = (s.loadString(), s.loadString());
  s.assertEnd();
  return (
    w.sb.0.equalTo("abcd"), w.sb.0.equalTo(s1),
    w.sb.1.equalTo("de"), w.sb.1.equalTo(s2),
  )
}

@method_id(119)
fun test19() {
  val C9 = "9".literalSlice();
  var (s9, s00_9, s01_9, s00_01_9) = (
    "9".beginParse(),
    "9".prefixWith00().beginParse(),
    "9".prefixWith01().beginParse(),
    "9".prefixWith00().prefixWith01().beginParse(),
  );
  return (
    s9.bitsEqual(C9),
    s00_9.bitsEqual(C9),
    s00_9.bitsEqual("00".hexToSlice()),
    s01_9.bitsEqual("01".hexToSlice()),
    s00_9.loadRef().beginParse().bitsEqual(C9),
    s00_01_9.loadRef().beginParse().loadRef().beginParse().bitsEqual(C9),
    "9".prefixWith00().equalTo("9"),
    "9".prefixWith00().equalTo("9".prefixWith00()),
    "9".prefixWith00().equalTo("9".prefixWith01()),
  )
}


/**
@testcase | 0   |     | 0
@testcase | 101 |     | [ 65 66 67 68 ]
@testcase | 102 |     | 0 1 4 -1 -1 -1 0
@testcase | 103 |     | 1016 0 1016 1 8 0 128
@testcase | 104 |     | -1
@testcase | 105 |     | 0 -1 4
@testcase | 106 |     | 4 16 1 -1
@testcase | 107 |     | 0 -1
@testcase | 108 | -1  | 9 5
@testcase | 108 | 0   | 8 4
@testcase | 109 |     | [ 97 98 99 100 101 102 103 104 ]
@testcase | 110 |     | 3 [ 48 49 50 ]
@testcase | 111 |     | [ 97 10 9 92 34 13 98 ] [ 97 10 34 34 98 ] 11 11 12 4 9 -1 13
@testcase | 112 |     | -1 -1 -1 -1 -1 31
@testcase | 113 |     | 27
@testcase | 114 |     | -1 0 0 0 -1 -1 0 -1 0 -1 0 -1 0 0 -1 -1 -1 0 0 -1 0 -1
@testcase | 116 |     | 43062 43062 43062 43062 43062
@testcase | 117 |     | 1633837924
@testcase | 118 |     | -1 -1 -1 -1
@testcase | 119 |     | -1 0 -1 -1 -1 -1 0 -1 0

@fif_codegen
"""
  test16() PROC:<{
    43062 PUSHINT
    s0 s0 s0 PUSH3
    DUP
  }>
"""
*/
