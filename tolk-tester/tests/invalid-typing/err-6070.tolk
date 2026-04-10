struct Wrapper<T> {
    value: T;
}

struct JustInt {
    value: int;
}

fun takeSmth<T>(obj: T) { return obj.value; }
fun takeW<T>(obj: Wrapper<T>) { return obj.value; }

fun cantPassJustIntAsWrapper() {
    var w: Wrapper<int> = { value: 10 };
    var j: JustInt = { value: 10 };

    takeSmth(w);
    takeSmth(j);
    takeW(w);
    takeW<int>(j);
}

/**
@compilation_should_fail
@stderr can not pass `JustInt` to `Wrapper<int>`
@stderr takeW<int>(j)
 */
