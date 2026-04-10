@overflow1023_policy("ignorje")
struct A {
    d1: bits256;
    d2: bits256;
    d3: bits256;
    d4: bits256;
    d5: bits256;
}

/**
@compilation_should_fail
@stderr incorrect value for @overflow1023_policy
 */
