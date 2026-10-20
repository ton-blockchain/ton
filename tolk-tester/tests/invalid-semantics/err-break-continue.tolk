// @compilation_should_fail

// @stderr `break` used outside a loop
// @stderr in function break_outside
fun break_outside() {
    break;
}

// @stderr `continue` used outside a loop
// @stderr in function continue_outside
fun continue_outside() {
    continue;
}

// @stderr `break` is not allowed inside `try/catch`
// @stderr in function break_in_try
fun break_in_try() {
    while (true) {
        try {
            break;
        } catch {
        }
    }
}

// @stderr `continue` is not allowed inside `try/catch`
// @stderr can not compile `continue`, because of `continue` in non-standard, unsupported position
// @stderr note: this prevents compiling `continue`
// @stderr in function continue_in_try
// @stderr try {
// @stderr continue  ;
fun continue_in_try() {
    while (true) {
        try {
            continue  ;
        } catch {
        }
    }
}

// @stderr can not compile `continue`, because `if/else` has complicated `continue` and multiple exit points
// @stderr in function unbalanced_if_continue
fun unbalanced_if_continue() {
    var a = true;
    var b = true;
    while (true) {
        if (a) {
            if (b) {
                continue;
            }
        }
        a = false;
    }
}

// @stderr can not compile `continue`, because some `match` arms do `continue`, some do not
// @stderr in function unbalanced_match_continue
fun unbalanced_match_continue(x: int) {
    while (true) {
        match (x) {
            0 => { continue; }
            1 => { x += 1; }
            else => { x += 2; }
        }
        x += 3;
    }
}

// @stderr can not compile `continue`, because a terminal `if/else/match` is followed by unreachable statements
// @stderr in function terminal_if_then_dead_suffix
fun terminal_if_then_dead_suffix() {
    while (true) {
        if (true) {
            continue;
        } else {
            break;
        }
        throw 1;
    }
}

// @stderr `return` is not allowed inside a loop that uses `break`
// @stderr in function return_and_break_same_loop
// @stderr } else {
// @stderr   return;
// @stderr note: `break` is used here
// @stderr if (cond) {
// @stderr   break;
fun return_and_break_same_loop(cond: bool) {
    while (true) {
        if (cond) {
            break;
        } else {
            return;
        }
    }
}

// @stderr `return` is not allowed in a loop condition
// @stderr in function return_in_while_cond
fun return_in_while_cond(): int {
    while (match (true) {
        true => {
            return 70;
            true
        }
        false => false
    }) {
    }
    return 0;
}

// @stderr in function break_in_try_then_return
fun break_in_try_then_return() {
    while (true) {
        try {
            break;
        } catch {
        }
        return;
    }
}

// @stderr in function continue_in_try_in_block
fun continue_in_try_in_block() {
    while (true) {
        {
            try {
                continue;
            } catch {
            }
        }
    }
}

// @stderr in function break_in_loop_nested_in_condition
fun break_in_loop_nested_in_condition() {
    while (match (0) {
        0 => {
            while (false) {
                break;
            }
            throw 1;
        }
        else => false
    }) {
    }
}

// @stderr in function continue_in_loop_nested_in_condition
fun continue_in_loop_nested_in_condition() {
    while (match (0) {
        0 => {
            while (false) {
                continue;
            }
            throw 1;
        }
        else => false
    }) {
    }
}

// @stderr in function return_in_repeat_count
fun return_in_repeat_count(): int {
    repeat (match (0) {
        0 => {
            return 5;
            1
        }
        else => 1
    }) {
    }
    return 0;
}

// @stderr in function return_in_nested_while_cond
fun return_in_nested_while_cond(flag: bool): int {
    while (true) {
        while (match (flag) {
            true => {
                return 71;
                true
            }
            false => false
        }) {
        }
        break;
    }
    return 91;
}

// @stderr in function bad_continue_after_terminal_throw
fun bad_continue_after_terminal_throw(x: int): int {
    while (x > 0) {
        throw 123;
        var z = match (x) {
            1 => { continue; 10 }
            else => 20
        };
        x = x - 1;
    }
    return x;
}
