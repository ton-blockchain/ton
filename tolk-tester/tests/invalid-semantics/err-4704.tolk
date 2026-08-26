enum AA {
    One = 1,
    Two,
    Three,
    Onne = 1,
}

enum BB {
    Zero,
    One,
    Two,
    Three,
    AlsoTwo = 2,
    AlsoThree,
}

/**
@compilation_should_fail
@stderr duplicate value `1` in enum `AA`
@stderr Onne = 1
@stderr One = 1
@stderr duplicate value `2` in enum `BB`
@stderr AlsoTwo
@stderr Two
@stderr duplicate value `3` in enum `BB`
@stderr AlsoThree
@stderr Three
 */
