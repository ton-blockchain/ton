struct Wrapper<T> {
    value: T;
}

fun analyze(w: Wrapper<int>) {}

type MInt = int;

fun main() {
    var w1: Wrapper<MInt> = { value: 10 };
    var w2 = Wrapper { value: 10 as int7 };

    analyze(w1);
    analyze(w2);
}

/**
@compilation_should_fail
@stderr can not pass `Wrapper<int7>` to `Wrapper<int>`
 */
