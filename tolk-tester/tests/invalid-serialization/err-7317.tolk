struct (0x1) A {}
struct B {}
struct C {}

type Msg = A | B | C

fun loadMsg(): Msg {
    return A.fromSlice(createEmptySlice());
}

fun main(): int {
    val msg = lazy loadMsg();
    return match (msg) {
        A => 1,
        B => 2,
        C => 3,
    };
}

/**
@compilation_should_fail
@stderr `lazy` operator can only be used with built-in functions like fromCell/fromSlice or simple wrappers over them
 */
