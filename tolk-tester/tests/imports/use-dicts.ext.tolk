import "@stdlib/tvm-dicts"

fun prepareDict_3_30_4_40_5_x(valueAt5: int): cell? {
    var dict: cell? = createEmptyDict();
    dict.iDictSetBuilder(32, 3, beginCell().storeInt(30, 32));
    dict.iDictSetBuilder(32, 4, beginCell().storeInt(40, 32));
    dict.iDictSetBuilder(32, 5, beginCell().storeInt(valueAt5, 32));
    return dict;
}

fun lookupIdxByValue(idict32: cell?, value: int): int {
    var cur_key: int? = -1;
    do {
        var (cur_key redef, cs: slice?, found: bool) = idict32.iDictGetNext(32, cur_key!);
        // one-line condition (via &) doesn't work, since right side is calculated immediately
        if (found) {
            if (cs!.loadInt(32) == value) {
                return cur_key!;
            }
        }
    } while (found);
    return -1;
}
