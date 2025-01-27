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
@stderr can not call method for `builder` with object of type `int`
 */
