fun cantUseSelfAsType(mutate x: int) {
    var y: self = x;
}

/**
@compilation_should_fail
@stderr `self` type can be used only as a return type of a method `fun T.methodForT(self)`
 */
