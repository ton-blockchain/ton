
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
    var c: cell = contract.getData();
    var cs: slice = c.beginParse();
    cs.loadBits(32);
    var value: int = cs.loadUint(16);
    return (1, value);
}

fun save_contract_data(value: int) {
    var b: builder = beginCell().storeInt(1, 32).storeUint(value, 16);
    contract.setData(b.endCell());
}

@pure
@method_id(101)
fun test1(): int {
    return f_pure1();
}

@method_id(102)
fun test2(value: int): int {
    save_contract_data(value);
    var (_, restored) = get_contract_data();
    return restored;
}

fun main() { return; }

/**

@testcase | 101 |     | 2
@testcase | 102 |  44 | 44

*/
