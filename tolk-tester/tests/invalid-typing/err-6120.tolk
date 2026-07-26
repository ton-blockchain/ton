fun int.bad(self): self {
    {
        var `self` = beginCell();
        return `self`;
    }
}

/**
@compilation_should_fail
@stderr invalid return from `self` function
 */
