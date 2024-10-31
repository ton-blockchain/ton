fun store_u32(b: builder, value: int): builder {
    return b.store_uint(value, 32);
}
fun ~store_u32(b: builder, value: int): (builder, ()) {
    return ~store_uint(b, value, 32);
}

fun load_u32(cs: slice): (slice, int) {
    return cs.load_uint(32);
}

fun my_load_int(s: slice, len: int): (slice, int)
    asm(s len -> 1 0) "LDIX";  // top is "value slice"
fun my_store_int(b: builder, x: int, len: int): builder
    asm(x b len) "STIX";
fun ~my_store_int(b: builder, x: int, len: int): (builder, ())
    asm(x b len) "STIX";

@method_id(101)
fun test1(): [int,int,int,int,int] {
    var b: builder = begin_cell().store_uint(1, 32);
    b = b.store_uint(2, 32);
    b~store_uint(3, 32);
    b = b.store_u32(4);
    b~store_u32(5);

    var cs: slice = b.end_cell().begin_parse();
    var (cs redef, one: int) = cs.load_uint(32);
    var (two: int, three: int) = (cs~load_uint(32), cs~load_u32());
    var (cs redef, four: int) = cs.load_u32();
    var five: int = cs~load_u32();

    return [one,two,three,four,five];
}

@method_id(102)
fun test2(): [int,int,int] {
    var b: builder = begin_cell().my_store_int(1, 32);
    b = b.my_store_int(2, 32);
    b~my_store_int(3, 32);

    var cs: slice = b.end_cell().begin_parse();
    var (cs redef, one: int) = cs.my_load_int(32);
    var (two: int, three: int) = (cs~my_load_int(32), cs~my_load_int(32));

    return [one,two,three];
}

@method_id(103)
fun test3(ret: int): int {
    var (_, same: int) = begin_cell().store_uint(ret,32).end_cell().begin_parse().load_uint(32);
    return same;
}

@method_id(104)
fun test4(): [int,int] {
    var b: builder = my_store_int(begin_cell(), 1, 32);
    b = store_int(store_int(b, 2, 32), 3, 32);

    var cs: slice = b.end_cell().begin_parse();
    var cs32: slice = cs.first_bits(32);      // todo s.first_bits()~load_uint() doesn't work, 'lvalue expected'
    var (one, _, three) = (cs32~load_int(32), cs~skip_bits(64), cs~load_u32());

    return [one,three];
}

@method_id(105)
fun test5(): [int,int] {
    var cref: cell = end_cell(store_u32(begin_cell(), 105));
    var c: cell = begin_cell().store_ref(cref).store_ref(cref).store_u32(1).end_cell();

    var cs: slice = begin_parse(c);
    // todo I want cs~load_ref().begin_parse()~load_u32(), but 'lvalue expected'
    var ref1 = cs~load_ref().begin_parse();
    var ref2 = cs~load_ref().begin_parse();
    var sto5x2: int = ref1~load_u32() + ref2~load_uint(32);
    return [sto5x2, cs~load_u32()];
}


fun ~sumNumbersInSlice(s: slice): (slice, int) {
    var result = 0;
    while (!slice_data_empty?(s)) {
        result += s~load_uint(32);
    }
    return (s, result);
}

@method_id(106)
fun test6() {
    var ref = begin_cell().store_int(100, 32).end_cell();
    var s: slice = begin_cell().store_int(1, 32).store_int(2, 32).store_ref(ref).end_cell().begin_parse();
    var result = (slice_bits(s), s~sumNumbersInSlice(), slice_bits(s), slice_empty?(s), slice_data_empty?(s), slice_refs_empty?(s));
    var ref2: cell = s~load_ref();
    var s2: slice = ref2.begin_parse();
    s.end_parse();
    return (result, s2~load_int(32), s2.slice_empty?());
}

@method_id(107)
fun test7() {
    var s: slice = begin_cell().store_int(1, 32).store_int(2, 32).store_int(3, 32).store_int(4, 32).store_int(5, 32).store_int(6, 32).store_int(7, 32).end_cell().begin_parse();
    var size1 = slice_bits(s);
    s~skip_bits(32);
    var s1: slice = s.first_bits(64);
    var n1 = s1~load_int(32);
    var size2 = slice_bits(s);
    s~load_int(32);
    var size3 = slice_bits(s);
    s~skip_last_bits(32);
    var size4 = slice_bits(s);
    var n2 = s~load_int(32);
    var size5 = slice_bits(s);
    return (n1, n2, size1, size2, size3, size4, size5);
}

@method_id(108)
fun test108() {
    var (result1, result2) = (0, 0);
    try {
        begin_cell().store_ref(begin_cell().end_cell()).end_cell().begin_parse().end_parse();
        result1 = 100;
    } catch (code) {
        result1 = code;
    }
    try {
        begin_cell().end_cell().begin_parse().end_parse();
        result2 = 100;
    } catch (code) {
        result2 = code;
    }
    return (result1, result2);
}

@method_id(109)
fun test109() {
    var ref2 = begin_cell().store_int(1, 32).end_cell();
    var ref1 = begin_cell().store_int(1, 32).store_ref(ref2).end_cell();
    var c = begin_cell().store_int(444, 32).store_ref(ref1).store_ref(ref1).store_ref(ref1).store_ref(ref2).store_int(4, 32).end_cell();
    var (n_cells1, n_bits1, n_refs1) = c.compute_data_size(10);
    var s = c.begin_parse();
    s~load_ref();
    s~load_ref();
    var n = s~load_int(32);
    var (n_cells2, n_bits2, n_refs2) = s.slice_compute_data_size(10);
    return ([n_cells1, n_bits1, n_refs1], [n_cells2, n_bits2, n_refs2], n);
}

fun main(): int {
    return 0;
}

/**
@testcase | 101 |     | [ 1 2 3 4 5 ]
@testcase | 102 |     | [ 1 2 3 ]
@testcase | 103 | 103 | 103
@testcase | 104 |     | [ 1 3 ]
@testcase | 105 |     | [ 210 1 ]
@testcase | 106 |     | 64 3 0 0 -1 0 100 -1
@testcase | 107 |     | 2 3 224 192 160 128 96
@testcase | 108 |     | 9 100
@testcase | 109 |     | [ 3 128 5 ] [ 2 96 3 ] 444
 */
