fun getBeginCell() {
    return beginCell;
}

fun getBeginParse() {
    return beginParse;
}

@method_id(101)
fun testVarApply1() {
    var (_, f_end_cell) = (0, endCell);
    var b: builder = (getBeginCell())().storeInt(1, 32);
    b.storeInt(2, 32);
    var s = (getBeginParse())(f_end_cell(b));
    return (s.loadInt(32), s.loadInt(32));
}

fun main() {}

/**
@testcase | 101 |    | 1 2
 */
