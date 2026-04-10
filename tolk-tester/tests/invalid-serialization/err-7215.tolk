type MInt = int;

struct CantBe {
    a: int8;
    b: MInt?;
}

struct Container<T> {
    item: T;
}

fun main(s: slice) {
    Container<CantBe>.fromSlice(s);
}

/**
@compilation_should_fail
@stderr auto-serialization via fromSlice() is not available for type `Container<CantBe>`
@stderr because field `Container<CantBe>.item` of type `CantBe` can't be serialized
@stderr because field `CantBe.b` of type `MInt?` can't be serialized
@stderr because alias `MInt` expands to `int`
@stderr because type `int` is not serializable
 */
