fun foo() { return 1; }

const asdf = 1 + foo();

/**
@compilation_should_fail
@stderr not a constant expression
@stderr const asdf
 */
