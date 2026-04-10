struct Wrapper<T> {
    value: T;
}

type IntOrWrapped8 = Wrapper<int8> | int;

fun main() {
    var w1: IntOrWrapped8 | Wrapper<int16> = Wrapper { value: 10 as int8 };
    var w2: IntOrWrapped8 | Wrapper<int16> = Wrapper { value: 10 as int16 };

    var w3: IntOrWrapped8 | Wrapper<int16> = Wrapper { value: 10 as int32 };
}

/**
@compilation_should_fail
@stderr can not assign `Wrapper<int32>` to variable of type `Wrapper<int8> | int | Wrapper<int16>`
@stderr var w3
 */
