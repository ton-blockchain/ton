fun int.incInt(mutate self): self {
    self += 1;
    return self;
}

fun builder.appendBuilder(mutate self): self {
    self.storeUint(1, 32);
    return self;
}

fun cantMixDifferentThis() {
    var x = 0;
    return x.incInt().appendBuilder().incInt();
}

/**
@compilation_should_fail
@stderr method `appendBuilder` not found for type `int`
 */
