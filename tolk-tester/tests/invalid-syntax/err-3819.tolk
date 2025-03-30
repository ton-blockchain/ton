fun main(flags: int): int {
    return flags&0xFF!=0;
}

/**
@compilation_should_fail
@stderr & has lower precedence than !=
@stderr Use parenthesis
*/
