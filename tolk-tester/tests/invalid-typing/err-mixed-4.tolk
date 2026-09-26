// @compilation_should_fail
// here are mixed errors from different compilation stages, to check they are fired all at once

enum Color { Red, Green, Blue }

// @stderr can not apply operator `+` to `Color` and `int`
fun errTypingOperator(c: Color) {
    c + 1;
}

// @stderr `break` used outside a loop
fun errBreakOutside() {
    break;
}

// @stderr function `errInlineReturnInLoop` can't be inlined
// @stderr `@inline` is impossible, because of `return` inside a loop
// @stderr note: this prevents inlining
// @stderr repeat (1) {
// @stderr return x; // here
@inline
fun errInlineReturnInLoop(x: int): int {
    repeat (1) {
        return x; // here
    }
    return 0;
}

// @stderr `continue` used outside a loop
fun errContinueInRepeatCount() {
    repeat (2) {
        repeat (match (true) {
            true => {
                continue;
                1
            }
            false => 1
        }) {
        }
    }
}

// @stderr `return` is not allowed in a loop condition
fun errReturnInDoWhileCond(takeBreak: bool, takeReturn: bool): int {
    do {
        if (takeBreak) {
            break;
        }
    } while (match (takeReturn) {
        true => {
            return 72;
            true
        }
        false => false
    });
    return 92;
}
