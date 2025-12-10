fun acceptsCb(cb: (int) -> void) {}

fun main() {
    // ok, int8 and int are assignable to each other
    acceptsCb(fun(x: int8) {});
    acceptsCb(fun(x: address) {});
}

/**
@compilation_should_fail
@stderr can not pass `(address) -> void` to `(int) -> void`
 */
