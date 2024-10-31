fun incInt(mutate self: int): self {
    self += 1;
    return self;
}

fun appendBuilder(mutate self: builder): self {
    self.storeUint(1, 32);
    return self;
}

fun cantMixDifferentThis() {
    var x = 0;
    return x.incInt().appendBuilder().incInt();
}

/**
@compilation_should_fail
@stderr cannot apply function appendBuilder : builder -> (builder, ()) to arguments of type int: cannot unify type int with builder
 */
