
@pure
fun f_pure1(): int {
    return f_pure2();
}

@pure
fun f_pure2(): int {
    return 2;
}

@pure
fun get_contract_data(): (int, int) {
    var c: cell = get_data();
    var cs: slice = c.begin_parse();
    cs~load_bits(32);
    var value: int = cs~load_uint(16);
    return (1, value);
}

fun save_contract_data(value: int) {
    var b: builder = begin_cell().store_int(1, 32).store_uint(value, 16);
    set_data(b.end_cell());
}

@pure
@method_id(101)
fun test1(): int {
    return f_pure1();
}

@method_id(102)
fun test2(value: int): int {
    save_contract_data(value);
    var (_, restored: auto) = get_contract_data();
    return restored;
}

fun main() { return; }

/**

@testcase | 101 |     | 2
@testcase | 102 |  44 | 44

*/
