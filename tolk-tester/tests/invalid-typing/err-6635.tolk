fun eq<T>(v: T): T { return v; }

fun main() {
    val p: (int, int) = (10, 20);
    eq(p).0 += 30;
}

/**
@compilation_should_fail
@stderr can not assign to a temporary expression
 */
