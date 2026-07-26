const `F<int>` = 0;

fun F<T>() {}

fun main() {
    F<int>();
}

/**
@compilation_should_fail
@stderr generic instantiation `F<int>` conflicts with existing symbol
 */
