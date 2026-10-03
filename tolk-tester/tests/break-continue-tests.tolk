fun main() {}

// --- basic break ---

@method_id(101)
fun break_immediate(): int {
    var x = 0;
    while (true) {
        break;
        x = 1;
    }
    return x;
}

@method_id(102)
fun break_after_side_effect(): int {
    var x = 0;
    while (true) {
        x = 7;
        break;
        x = 9;
    }
    return x;
}

@method_id(103)
fun break_conditional(flag: int): int {
    var x = 0;
    while (x < 10) {
        if (flag != 0) {
            break;
        }
        x += 1;
    }
    return x;
}

@method_id(104)
fun break_nested_if(n: int): int {
    var i = 0;
    while (i < 20) {
        if (i > 3) {
            if (i == n) {
                break;
            }
        }
        i += 1;
    }
    return i;
}

@method_id(105)
fun break_nested_loops(): int {
    var outer = 0;
    var inner = 0;
    while (outer < 5) {
        inner = 0;
        while (inner < 5) {
            if (inner == 2) {
                break;
            }
            inner += 1;
        }
        outer += 1;
    }
    return outer * 10 + inner;
}

@method_id(106)
fun while_true_break_reaches_next(): int {
    while (true) {
        break;
    }
    return 42;
}

// --- structural continue ---

@method_id(107)
fun continue_direct(): int {
    var i = 0;
    var n = 0;
    while (i < 5) {
        i += 1;
        continue;
        n += 1;
    }
    return n * 10 + i;
}

@method_id(108)
fun continue_one_guard(): int {
    var i = 0;
    var n = 0;
    while (i < 6) {
        i += 1;
        if ((i & 1) == 0) {
            continue;
        }
        n += 1;
    }
    return n;
}

@method_id(109)
fun continue_two_guards(): int {
    var i = 0;
    var n = 0;
    while (i < 10) {
        i += 1;
        if (i == 3) {
            continue;
        }
        if (i == 7) {
            continue;
        }
        n += i;
    }
    return n;
}

@method_id(110)
fun continue_balanced_nested(): int {
    var i = 0;
    var n = 0;
    while (i < 5) {
        i += 1;
        if (i == 2) {
            continue;
        } else {
            if (i == 4) {
                continue;
            } else {
                n += i;
            }
        }
    }
    return n;
}

@method_id(111)
fun continue_match(x: int): int {
    var i = 0;
    var n = 0;
    while (i < 4) {
        i += 1;
        match (i) {
            2 => { continue; }
            else => { n += i; }
        }
    }
    return n + x;
}

@method_id(112)
fun continue_then_break(limit: int): int {
    var i = 0;
    var n = 0;
    while (true) {
        i += 1;
        if (i == 2) {
            continue;
        }
        n += i;
        if (i >= limit) {
            break;
        }
    }
    return n;
}

@method_id(113)
fun continue_then_return(limit: int): int {
    var i = 0;
    while (i < 10) {
        i += 1;
        if (i == 2) {
            continue;
        }
        if (i == limit) {
            return i * 10;
        }
    }
    return -1;
}

// --- do-while: continue evaluates cond, break does not ---

global do_cond_hits: int;

fun do_cond(n: int): bool {
    do_cond_hits += 1;
    return n < 3;
}

@method_id(114)
fun do_while_continue(): int {
    do_cond_hits = 0;
    var n = 0;
    do {
        n += 1;
        if (n == 1) {
            continue;
        }
    } while (do_cond(n));
    return n * 10 + do_cond_hits;
}

@method_id(115)
fun do_while_break(): int {
    do_cond_hits = 0;
    var n = 0;
    do {
        n += 1;
        if (n == 1) {
            break;
        }
    } while (do_cond(n));
    return n * 10 + do_cond_hits;
}

@method_id(116)
fun do_while_fallthrough(): int {
    do_cond_hits = 0;
    var n = 0;
    do {
        n += 1;
    } while (do_cond(n));
    return n * 10 + do_cond_hits;
}

// --- repeat ---

global repeat_count_hits: int;

fun repeat_count(): int {
    repeat_count_hits += 1;
    return 3;
}

@method_id(117)
fun repeat_count_once(): int {
    repeat_count_hits = 0;
    var n = 0;
    repeat (repeat_count()) {
        n += 1;
    }
    return n * 10 + repeat_count_hits;
}

@method_id(118)
fun repeat_zero(): int {
    var n = 0;
    repeat (0) {
        n += 1;
        break;
    }
    return n;
}

@method_id(119)
fun repeat_negative(): int {
    var n = 0;
    repeat (-5) {
        n += 1;
        break;
    }
    return n;
}

@method_id(120)
fun repeat_continue_consumes(): int {
    var n = 0;
    var skipped = 0;
    repeat (4) {
        n += 1;
        if (n == 2) {
            skipped += 1;
            continue;
        }
        skipped += 10;
    }
    return n * 100 + skipped;
}

@method_id(121)
fun repeat_break_stops(): int {
    var n = 0;
    repeat (10) {
        n += 1;
        if (n == 3) {
            break;
        }
    }
    return n;
}

// --- return is allowed when the surrounding loop has no break ---

@method_id(122)
fun inner_break_then_outer_return(): int {
    var n = 0;
    while (n < 3) {
        while (true) {
            n += 1;
            break;
        }
        if (n == 2) {
            return 90;
        }
    }
    return 91;
}

@method_id(125)
fun return_from_try_in_loop(flag: int): int {
    var i = 0;
    while (i < 3) {
        try {
            if (flag != 0) {
                return 55;
            }
        } catch (excNo, arg) {
            return 66;
        }
        i += 1;
    }
    return 77;
}

// --- FlowContext smart casts ---

@method_id(128)
fun flow_break_nullable(x: int?, flag: int): int {
    while (x != null) {
        __expect_type(x, "int");
        if (flag != 0) {
            break;
        }
        x = null;
    }
    __expect_type(x, "int?");
    return x == null ? 0 : x;
}

@method_id(130)
fun break_nested_match(n: int): int {
    var i = 0;
    while (i < 20) {
        match (i == n) {
            true => { break; }
            else => { i += 1; }
        }
    }
    return i;
}

@method_id(131)
fun continue_match_implicit_else(): int {
    var i = 0;
    var n = 0;
    while (i < 4) {
        i += 1;
        match (i) {
            2 => { continue; }
        }
        n += i;
    }
    return n;
}

@method_id(132)
fun continue_then_throw(flag: int): int {
    var i = 0;
    while (i < 5) {
        i += 1;
        if (i == 2) {
            continue;
        }
        if (flag != 0 && i == 3) {
            throw 99;
        }
    }
    return i;
}

fun generic_break<T>(x: T): T {
    while (true) {
        break;
    }
    return x;
}

@method_id(133)
fun test_generic_break(): int {
    return generic_break(7);
}

@inline
fun add1(x: int): int {
    return x + 1;
}

@method_id(134)
fun inline_inside_breakable(): int {
    while (true) {
        var x = add1(10);
        if (x == 11) {
            break;
        }
    }
    return 11;
}

@method_id(136)
fun continue_nested_loops(): int {
    var outer = 0;
    var n = 0;
    while (outer < 3) {
        outer += 1;
        var inner = 0;
        while (inner < 3) {
            inner += 1;
            if (inner == 2) {
                continue;
            }
            n += 1;
        }
        n += 10;
    }
    return n;
}

@method_id(129)
fun flow_continue_nullable(x: int?, flag: int): int {
    var n = 0;
    while (x != null) {
        __expect_type(x, "int");
        if (flag != 0) {
            flag = 0;
            continue;
        }
        n += x;
        x = null;
    }
    __expect_type(x, "null");
    return n;
}

@method_id(140)
fun again_then_return(x: int): int {
    if (x > 0) {
        while (true) {
            if (x == 1) {
                break;
            }
            x -= 1;
        }
        return 10;
    }
    return 20;
}

@method_id(141)
fun again_late_init_break(): int {
    var x: int;
    while (true) {
        x = 1;
        break;
    }
    return x;
}

@method_id(142)
fun continue_after_not(flag: int): int {
    var x = 0;
    var ok = flag != 0;
    repeat (2) {
        if (!ok) {
            continue;
        }
        x += 1;
    }
    return x;
}

@method_id(144)
fun do_while_continue_or_break(flag: int): int {
    do_cond_hits = 0;
    var n = 0;
    do {
        n += 1;
        if (flag != 0) {
            continue;
        } else {
            break;
        }
    } while (do_cond(n));
    return n * 10 + do_cond_hits;
}

fun generic_continue<T>(x: T, skip: int): T {
    var i = 0;
    while (i < 2) {
        i += 1;
        if (skip != 0) {
            continue;
        }
    }
    return x;
}

@method_id(145)
fun test_generic_continue(): int {
    return generic_continue(7, 1);
}

@inline
fun skip_neg(x: int): int {
    if (x < 0) {
        return 0;
    }
    return x;
}

@method_id(146)
fun continue_with_inline_guard(): int {
    var i = 0;
    var n = 0;
    while (i < 4) {
        i += 1;
        if (skip_neg(i - 3) == 0) {
            continue;
        }
        n += i;
    }
    return n;
}

@method_id(148)
fun continue_then_terminal(a: int, b: int): int {
    var n = 0;
    repeat (2) {
        if (a != 0) {
            if (b != 0) {
                continue;
            }
            break;
        } else {
            n += 1;
        }
        n += 10;
    }
    return n;
}

@method_id(149)
fun again_brk_backedge(n: int): int {
    var x = n;
    var done = 0;
    while (true) {
        if (x <= 0) {
            done = 1;
            break;
        }
        x -= 1;
    }
    return x * 10 + done;
}

@method_id(150)
fun while_brk_const_tags(flag: int): (int, int) {
    var x = 5;
    var i = 0;
    while (i < 1) {
        if (flag != 0) {
            x = 9;
            break;
        }
        i += 1;
    }
    return (x, 5);
}

@method_id(151)
fun until_brk_const_tags(flag: int): (int, int) {
    var x = 5;
    var i = 0;
    do {
        if (flag != 0) {
            x = 9;
            break;
        }
        i += 1;
    } while (i < 1);
    return (x, 5);
}

@method_id(154)
fun both_arms_terminal_then_continue(a: int): int {
    var n = 0;
    repeat (3) {
        if (a != 0) {
            break;
        } else {
            throw 1;
        }
        if (true) {
            continue;
        }
        n += 10;
    }
    return n;
}

@method_id(155)
fun match_break_no_continue(a: int, x: int): int {
    var n = 0;
    while (true) {
        if (a != 0) {
            match (x) {
                0 => { break; }
            }
        } else {
            n += 1;
        }
        n += 10;
        break;
    }
    return n;
}

@method_id(157)
fun nested_both_breakable(): int {
    var outer = 0;
    var inner = 0;
    while (outer < 3) {
        inner = 0;
        while (inner < 5) {
            if (inner == 2) {
                break;
            }
            inner += 1;
        }
        if (outer == 1) {
            break;
        }
        outer += 1;
    }
    return outer * 10 + inner;
}

@inline
fun early_ret_helper(x: int): int {
    if (x < 0) {
        return 0;
    }
    return x;
}

@method_id(158)
fun inline_helper_in_breakable(x: int): int {
    var n = 0;
    while (true) {
        n = early_ret_helper(x);
        break;
    }
    return n;
}

@method_id(159)
fun try_then_break(flag: int): int {
    var n = 0;
    while (true) {
        try {
            if (flag != 0) {
                throw 1;
            }
            n = 7;
        } catch {
            n = 8;
        }
        break;
    }
    return n;
}

fun add3_ref(x: int): int {
    return x + 3;
}

@method_id(160)
fun again_funref_callxargs(): int {
    var cb: (int) -> int = add3_ref;
    var x = 1;
    while (true) {
        x = cb(x);
        break;
    }
    return x;
}

@method_id(161)
fun again_cond_lateinit(n: int, seed: int): int {
    var done: int;
    var x = n;
    while (true) {
        if (x <= 0) {
            done = seed + x;
            break;
        }
        x -= 1;
    }
    return done;
}

// --- lazy match: same FallthroughTail routing as a regular match ---

struct (0x00) LazyBcA { a: int8 }
struct (0x10) LazyBcB { b: int8 }
type LazyBcU = LazyBcA | LazyBcB

@method_id(162)
fun lazy_continue_one_arm(flag: int): int {
    var n = 0;
    var i = 0;
    while (i < 3) {
        i += 1;
        val s = (i == 2) == (flag != 0) ? "0005".hexToSlice() : "1007".hexToSlice();
        var x = lazy LazyBcU.fromSlice(s);
        __expect_lazy("[x] lazy match");
        match (x) {
            LazyBcA => { continue; }
            LazyBcB => { n += x.b; }
        }
        n += 10;
    }
    return n;
}

@method_id(163)
fun lazy_break_one_arm(flag: int): int {
    var n = 0;
    while (true) {
        val s = flag != 0 ? "0005".hexToSlice() : "1007".hexToSlice();
        var x = lazy LazyBcU.fromSlice(s);
        match (x) {
            LazyBcA => {
                n = x.a;
                break;
            }
            LazyBcB => { n = x.b; }
        }
        n += 100;
        break;
    }
    return n;
}

@method_id(164)
fun lazy_continue_then_break(): int {
    var i = 0;
    var n = 0;
    while (true) {
        i += 1;
        val s = i < 3 ? "0005".hexToSlice() : "1007".hexToSlice();
        var x = lazy LazyBcU.fromSlice(s);
        match (x) {
            LazyBcA => {
                n += 1;
                continue;
            }
            LazyBcB => {
                n += 10;
                break;
            }
        }
    }
    return n * 10 + i;
}

@method_id(165)
fun lazy_else_break(flag: int): int {
    var n = 0;
    while (true) {
        val s = flag != 0 ? "0005".hexToSlice() : "20FF".hexToSlice();
        var x = lazy LazyBcU.fromSlice(s);
        match (x) {
            LazyBcA => {
                n = x.a;
                break;
            }
            LazyBcB => {
                n = x.b;
                break;
            }
            else => {
                n = 99;
                break;
            }
        }
    }
    return n;
}

@method_id(166)
fun lazy_else_continue(): int {
    var i = 0;
    var n = 0;
    while (i < 3) {
        i += 1;
        val s = i == 2 ? "20FF".hexToSlice() : "0005".hexToSlice();
        var x = lazy LazyBcU.fromSlice(s);
        match (x) {
            LazyBcA => { n += x.a; }
            LazyBcB => { continue; }
            else => { continue; }
        }
        n += 10;
    }
    return n;
}

@inline
fun lazy_inline_read(s: slice): int {
    __expect_inline(true);
    var x = lazy LazyBcU.fromSlice(s);
    match (x) {
        LazyBcA => { return x.a; }
        LazyBcB => { return x.b; }
        else => { return -1; }
    }
}

@method_id(167)
fun loop_calls_inline_lazy(kind: int): int {
    var n = 0;
    var i = 0;
    while (i < 3) {
        i += 1;
        val s = kind == 0 ? "0005".hexToSlice() : kind == 1 ? "1007".hexToSlice() : "20FF".hexToSlice();
        val v = lazy_inline_read(s);
        if (v < 0) {
            continue;
        }
        n += v;
        break;
    }
    return n * 10 + i;
}

@inline
fun lazy_inline_skip_a(sA: slice, sB: slice): int {
    __expect_inline(true);
    var n = 0;
    var i = 0;
    while (i < 2) {
        i += 1;
        val s = i == 1 ? sA : sB;
        var x = lazy LazyBcU.fromSlice(s);
        match (x) {
            LazyBcA => { continue; }
            LazyBcB => { n += x.b; }
        }
        n += 10;
    }
    return n;
}

@method_id(168)
fun test_lazy_inline_continue(): int {
    return lazy_inline_skip_a("0005".hexToSlice(), "1007".hexToSlice());
}

@method_id(171)
fun break_from_match_expression(x: int): int {
    var result = 10;
    while (true) {
        result = match (x) {
            0 => {
                break;
                20
            }
            else => 30
        };
        break;
    }
    return result;
}

@method_id(172)
fun flow_merge_reachable_breaks(flag: int): int {
    var x: int;
    while (true) {
        if (flag == 1) {
            x = 4;
            break;
        }
        if (flag == 2) {
            x = 7;
            break;
        }
        throw 100;
        break;
    }
    return x;
}

@method_id(173)
fun flow_continue_into_do_condition(flag: bool): int {
    var x: int;
    do {
        if (flag) {
            x = 4;
            continue;
        }
        x = 7;
    } while (x < 0);
    return x;
}

// An all-terminal inner `if` must not be counted as a fallthrough path.
@method_id(174)
fun continue_then_all_terminal_if(outer: int, skip: int, exitKind: int): int {
    var n = 0;
    repeat (3) {
        n += 1;
        if (outer != 0) {
            n += 10;
            if (skip != 0) {
                continue;
            }
            if (exitKind != 0) {
                n += 1000;
                break;
            } else {
                n += 2000;
                break;
            }
        } else {
            n += 100;
            if (skip != 0) {
                continue;
            }
            if (exitKind != 0) {
                n += 3000;
                break;
            } else {
                n += 4000;
                break;
            }
        }
    }
    return n;
}

// An exhaustive all-terminal inner `match` must not be counted as a live arm.
@method_id(175)
fun continue_then_all_terminal_match(outer: int, skip: int, exitKind: int): int {
    var n = 0;
    repeat (3) {
        n += 1;
        match (outer) {
            0 => {
                n += 10;
                if (skip != 0) {
                    continue;
                }
                match (exitKind) {
                    0 => {
                        n += 1000;
                        break;
                    }
                    else => {
                        n += 2000;
                        break;
                    }
                }
            }
            else => {
                n += 100;
                if (skip != 0) {
                    continue;
                }
                match (exitKind) {
                    0 => {
                        n += 3000;
                        break;
                    }
                    else => {
                        n += 4000;
                        break;
                    }
                }
            }
        }
    }
    return n;
}

// A break from the nested loop must reach the return materialized at the end of the outer IF branch.
@method_id(176)
fun breakable_loop_in_if_before_return(flag: int): int {
    var n = 0;
    if (flag != 0) {
        while (true) {
            n = 7;
            break;
        }
    } else {
        n = 9;
    }
    return n;
}

@inline
fun returnOrLoopForever(c: bool): int {
    if (c) {
        return -1;
    }
    while (true) {
    }
    return 1;
}

@method_id(177)
fun testReturnPath(c: bool): int {
    return returnOrLoopForever(c);
}


@method_id(178)
fun while_break_before_unused_copy(v: int): int {
    while (v > 10) {
        break;
    }
    var w = v;
    return 0;
}

/**
@testcase | 101 | | 0
@testcase | 102 | | 7
@testcase | 103 | 1 | 0
@testcase | 103 | 0 | 10
@testcase | 104 | 6 | 6
@testcase | 105 | | 52
@testcase | 106 | | 42
@testcase | 107 | | 5
@testcase | 108 | | 3
@testcase | 109 | | 45
@testcase | 110 | | 9
@testcase | 111 | 0 | 8
@testcase | 112 | 4 | 8
@testcase | 113 | 4 | 40
@testcase | 114 | | 33
@testcase | 115 | | 10
@testcase | 116 | | 33
@testcase | 117 | | 31
@testcase | 118 | | 0
@testcase | 119 | | 0
@testcase | 120 | | 431
@testcase | 121 | | 3
@testcase | 122 | | 90
@testcase | 125 | 1 | 55
@testcase | 125 | 0 | 77
@testcase | 128 | 5 1 | 5
@testcase | 128 | 5 0 | 0
@testcase | 129 | 5 1 | 5
@testcase | 129 | 5 0 | 5
@testcase | 130 | 6 | 6
@testcase | 131 | | 8
@testcase | 132 | 0 | 5
@testcase | 133 | | 7
@testcase | 134 | | 11
@testcase | 136 | | 36
@testcase | 140 | 3 | 10
@testcase | 140 | 1 | 10
@testcase | 140 | 0 | 20
@testcase | 141 | | 1
@testcase | 142 | 0 | 0
@testcase | 142 | 1 | 2
@testcase | 144 | 1 | 33
@testcase | 144 | 0 | 10
@testcase | 145 | | 7
@testcase | 146 | | 4
@testcase | 148 | 1 1 | 0
@testcase | 148 | 1 0 | 0
@testcase | 148 | 0 0 | 22
@testcase | 149 | 2 | 1
@testcase | 150 | 1 | 9 5
@testcase | 150 | 0 | 5 5
@testcase | 151 | 1 | 9 5
@testcase | 151 | 0 | 5 5
@testcase | 154 | 1 | 0
@testcase | 155 | 1 0 | 0
@testcase | 155 | 1 1 | 10
@testcase | 155 | 0 0 | 11
@testcase | 157 | | 12
@testcase | 158 | 5 | 5
@testcase | 158 | -3 | 0
@testcase | 159 | 0 | 7
@testcase | 159 | 1 | 8
@testcase | 160 | | 4
@testcase | 161 | 0 7 | 7
@testcase | 161 | 2 7 | 7
@testcase | 161 | -3 7 | 4
@testcase | 162 | 1 | 34
@testcase | 162 | 0 | 17
@testcase | 163 | 1 | 5
@testcase | 163 | 0 | 107
@testcase | 164 | | 123
@testcase | 165 | 1 | 5
@testcase | 165 | 0 | 99
@testcase | 166 | | 30
@testcase | 167 | 0 | 51
@testcase | 167 | 1 | 71
@testcase | 167 | 2 | 3
@testcase | 168 | | 17
@testcase | 171 | 0 | 10
@testcase | 171 | 1 | 30
@testcase | 172 | 1 | 4
@testcase | 172 | 2 | 7
@testcase | 173 | 0 | 7
@testcase | 173 | 1 | 4
@testcase | 174 | 1 1 0 | 33
@testcase | 174 | 0 1 0 | 303
@testcase | 174 | 1 0 1 | 1011
@testcase | 174 | 1 0 0 | 2011
@testcase | 174 | 0 0 1 | 3101
@testcase | 174 | 0 0 0 | 4101
@testcase | 175 | 0 1 0 | 33
@testcase | 175 | 1 1 0 | 303
@testcase | 175 | 0 0 0 | 1011
@testcase | 175 | 0 0 1 | 2011
@testcase | 175 | 1 0 0 | 3101
@testcase | 175 | 1 0 1 | 4101
@testcase | 176 | 0 | 9
@testcase | 176 | 1 | 7
@testcase | 177 | 1 | -1
@testcase | 178 | 5 | 0
@testcase | 178 | 15 | 0

@fif_codegen
"""
break_immediate() PROC:<{
  ...
  AGAINBRK:<{
  ...
  RETALT
  ...
}>
"""

@fif_codegen
"""
continue_one_guard() PROC:<{
  ...
  WHILE:<{
  ...
}>
"""

@fif_codegen_avoid _Continue
@fif_codegen_avoid Kcontinue

@fif_codegen_avoid JMPX
@fif_codegen_avoid
"""
break_immediate() PROC:<{
  c2 SAVE
"""

@fif_codegen
"""
break_conditional() PROC:<{
  ...
  WHILEBRK:<{
  ...
  RETALT
  ...
}>
"""

@fif_codegen
"""
while_brk_const_tags() PROC:<{
  ...
  WHILEBRK:<{
  ...
}>
"""

@fif_codegen
"""
until_brk_const_tags() PROC:<{
  ...
  UNTILBRK:<{
  ...
}>
"""

@fif_codegen
"""
do_while_break() PROC:<{
  ...
  UNTILBRK:<{
  ...
  RETALT
  ...
}>
"""

@fif_codegen
"""
repeat_break_stops() PROC:<{
  ...
  REPEATBRK:<{
  ...
  RETALT
  ...
}>
"""

@fif_codegen
"""
repeat_continue_consumes() PROC:<{
  ...
  REPEAT:<{
  ...
}>
"""

@fif_codegen
"""
do_while_continue() PROC:<{
  ...
  UNTIL:<{
  ...
}>
"""

@fif_codegen
"""
breakable_loop_in_if_before_return() PROC:<{
  IFJMP:<{
    AGAINBRK:<{
      7 PUSHINT
      RETALT
    }>
  }>
  9 PUSHINT
}>
"""
*/
