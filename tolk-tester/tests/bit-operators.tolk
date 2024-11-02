fun lshift(): int {
    return (1 << 0) == 1;
}

fun rshift(): int {
    return (1 >> 0) == 1;
}

fun lshift_var(i: int): int {
    return (1 << i) == 1;
}

fun rshift_var(i: int): int {
    return (1 >> i) == 1;
}

fun main(x: int): int {
    if (x == 0) {
        return lshift();
    } else if (x == 1) {
        return rshift();
    } else if (x == 2) {
        return lshift_var(0);
    } else if (x == 3) {
        return rshift_var(0);
    } else if (x == 4) {
        return lshift_var(1);
    } else {
        return rshift_var(1);
    }
}

@method_id(11)
fun is_claimed(index: int): int {
    var claim_bit_index: int = index % 256;
    var mask: int = 1 << claim_bit_index;
    return (255 & mask) == mask;
}


/**
    method_id | in  | out
@testcase | 0  | 0   | -1
@testcase | 0  | 1   | -1
@testcase | 0  | 2   | -1
@testcase | 0  | 3   | -1
@testcase | 0  | 4   |  0
@testcase | 0  | 5   |  0
@testcase | 11 | 0   | -1
@testcase | 11 | 1   | -1
@testcase | 11 | 256 | -1
@testcase | 11 | 8   |  0
*/
