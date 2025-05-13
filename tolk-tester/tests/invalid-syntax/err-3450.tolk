struct Wrapper<T> {
    value: T;
}

fun main() {
    var w3: Wrapper<Wrapper<Wrapper<int>> = {};
}

/**
@compilation_should_fail
@stderr expected `>` or `,`, got `=`
 */
