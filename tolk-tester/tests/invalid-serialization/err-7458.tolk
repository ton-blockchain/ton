struct CustomValue {
    value: int8
}

fun CustomValue.unpackFromSlice(mutate s: slice): CustomValue {
    return { value: s.loadInt(8) as int8 };
}

fun main() {
    var m: map<int8, CustomValue> = [];
    val v: CustomValue = { value: 7 };
    m.set(1 as int8, v);
    return 0;
}

/**
@compilation_should_fail
@stderr invalid `map`: type `CustomValue` can not be used as a value
@stderr because type `CustomValue` defines a custom unpack function, but does not define pack
 */
