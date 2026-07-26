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
    var cur_found: bool;
    do {
        var (new_key, cs: slice?, found: bool) = idict32.iDictGetNext(32, cur_key!);
        cur_key = new_key;
        cur_found = found;
        if (cur_found) {
            if (cs!.preloadInt(32) == value) {
                return cur_key!;
            }
        }
    } while (cur_found);
    return -1;
}
