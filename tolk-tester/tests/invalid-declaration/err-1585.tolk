struct B { a: A }
struct A { b: BAlias }

type BAlias = B;

/**
@compilation_should_fail
@stderr struct `B` size is infinity due to recursive fields
@stderr struct B {
 */
