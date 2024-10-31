fun prepareDict_3_30_4_40_5_x(valueAt5: int): cell {
    var dict: cell = createEmptyDict();
    dict.idict_set_builder(32, 3, begin_cell().store_int(30, 32));
    dict.idict_set_builder(32, 4, begin_cell().store_int(40, 32));
    dict.idict_set_builder(32, 5, begin_cell().store_int(valueAt5, 32));
    return dict;
}

fun lookupIdxByValue(idict32: cell, value: int): int {
    var cur_key = -1;
    do {
        var (cur_key redef, cs: slice, found: int) = idict32.idictGetNext(32, cur_key);
        // one-line condition (via &) doesn't work, since right side is calculated immediately
        if (found) {
            if (cs.loadInt(32) == value) {
                return cur_key;
            }
        }
    } while (found);
    return -1;
}
