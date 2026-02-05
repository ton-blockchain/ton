fun acceptsCb(cb: (int) -> void) {}

fun main() {
    acceptsCb(fun(x: int8) {});     // ok, int8 and int are assignable to each other
    acceptsCb(fun(x: address) {});
}

/**
@compilation_should_fail
@stderr can not pass `(address) -> void` to `(int) -> void`
@stderr_avoid ok
 */
