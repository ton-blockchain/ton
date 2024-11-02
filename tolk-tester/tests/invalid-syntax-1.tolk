fun main(x: int): int {
    if (x > 0) {
        return 1;
    }
    // 'elseif' doesn't exist anymore, it's treated as 'someFunction(arg)'
    elseif(x < 0) {
        return -1;
    }
    return x;
}

/**
@compilation_should_fail
@stderr expected `;`, got `{`
 */
