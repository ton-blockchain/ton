// @compilation_should_fail

// @stderr function `badInlineCalledFromUnused` can't be inlined
// @stderr `@inline` is impossible, because of `return` inside a loop
@inline
fun badInlineCalledFromUnused(x: int): int {
    repeat (1) {
        return x;
    }
    return 0;
}

// @stderr function `badInlineRecursionA` can't be inlined
// @stderr `@inline` is impossible, because a function recursively calls itself
@inline
fun badInlineRecursionA(x: int): int {
    if (x <= 0) {
        return 0;
    }
    return badInlineRecursionB(x - 1);
}

fun badInlineRecursionB(x: int): int {
    return badInlineRecursionA(x);
}

// @stderr function `badInlineMatch` can't be inlined
@inline
fun badInlineMatch(x: int): int {
    return match (x) {
        0 => return 0,
        else => 1,
    };
}

// @stderr function `badInlineMatchStatement` can't be inlined
// @stderr `@inline` is impossible, because some `match` arms do `return`, some do not
@inline
fun badInlineMatchStatement(x: int | slice): int {
    match (x) {
        int => {
            if (x == 0) {
                return 1;
            }
        }
        slice => {},
    }
    return 0;
}

// @stderr function `badInlinePartialIf` can't be inlined
// @stderr `@inline` is impossible, because `if/else` has complicated `return` and multiple exit points
@inline
fun badInlinePartialIf(x: int): int {
    if (x > 0) {
        if (x == 1) {
            return 1;
        }
    }
    return 2;
}

// @stderr function `badInlineReturnInIfCond` can't be inlined
@inline
fun badInlineReturnInIfCond(x: int): int {
    if ((match (x) { 0 => return 9, else => 1 }) > 0) {
        return 2;
    }
    return 3;
}

fun consume(a: int): void {}

// @stderr function `badInlineReturnInCallArg` can't be inlined
// @stderr `@inline` is impossible, because of `return` in non-standard, unsupported position
@inline
fun badInlineReturnInCallArg(x: int): int {
    consume(match (x) { 0 => return 1, else => 2 });
    return 0;
}

// @stderr function `badInlineTry` can't be inlined
// @stderr `@inline` is impossible, because of `return` inside try/catch
@inline
fun badInlineTry(x: int): int {
    try {
        return x;
    } catch {
        return 0;
    }
}

// @stderr function `badInlineReference` can't be inlined
// @stderr `@inline` is impossible, because it's used as a reference
@inline
fun badInlineReference(): int {
    return 1;
}

// @stderr function `onBouncedMessage` can't be inlined
@inline
fun onBouncedMessage(in: InMessageBounced): void {
    while (true) {
        return;
    }
}

fun onInternalMessage(in: InMessage) {
    badInlineReference;
    return badInlineReference();
}

// @stderr function `badInlineMethodId` can't be inlined
// @stderr `@inline` is impossible, because `@inline` is incompatible with `@method_id` and `get fun`
@inline
@method_id(12345)
fun badInlineMethodId(): int {
    return 1;
}

// @stderr function `badInlineStandaloneBlock` can't be inlined
@inline
fun badInlineStandaloneBlock(x: int): int {
    {
        return x;
    }
    return 0;
}

// @stderr function `badInlineReachableAfterTerminalIfElse` can't be inlined
// @stderr `@inline` is impossible, because a terminal `if/else/match` is followed by unreachable statements
@inline
fun badInlineReachableAfterTerminalIfElse(flag: int): int {
    if (flag != 0) {
        return 1;
    } else {
        return 2;
    }
    debug.print(99);
}

// @stderr function `badInlineReachableAfterTerminalMatch` can't be inlined
@inline
fun badInlineReachableAfterTerminalMatch(x: int) {
    match (x) {
        -1 => return -1,
        0  => return 0,
        else => return 100500,
    }
    return -100;
}

// @stderr function `badInlineAfterTerminalThrow` can't be inlined
@inline
fun badInlineAfterTerminalThrow(x: int): int {
    throw 123;
    var z = match (x) {
        1 => { return 10; 10 }
        else => 20
    };
    return z;
}
