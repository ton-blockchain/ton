fun unused1(): int { return 2; }
fun unused2(): int { return unused1(); }
fun unused3(x: int): int { return x * 2+unused2(); }

fun used_from_noncall1(): int { return 10; }
fun used_as_noncall1(): int { return used_from_noncall1(); }

const int20: int = 20;
fun used_from_noncall2(): int { return int20; }
fun used_as_noncall2(): int { return 0 * 0 + used_from_noncall2() + (0 << 0); }

global unused_gv: int;
global used_gv: int;

fun receiveGetter(): () -> int { return used_as_noncall2; }

@pure
fun usedButOptimizedOut(x: int): int { return x + 2; }

fun main(): (int, int, int) {
    used_gv = 1;
    used_gv = used_gv + 2;
    var getter1 = used_as_noncall1;
    var getter2 = receiveGetter();
    usedButOptimizedOut(used_gv);
    return (used_gv, getter1(), getter2());
}

/**
@experimental_options remove-unused-functions

@testcase | 0 | | 3 10 20

@fif_codegen DECLPROC used_as_noncall1
@fif_codegen DECLGLOBVAR used_gv

@fif_codegen_avoid DECLPROC unused1
@fif_codegen_avoid DECLPROC unused2
@fif_codegen_avoid DECLPROC unused3
@fif_codegen_avoid DECLGLOBVAR unused_gv

Note, that `usedButOptimizedOut()` (a pure function which result is unused)
is currently codegenerated, since it's formally reachable.
This is because optimizing code is a moment of codegen for now (later than marking unused symbols).

@fif_codegen DECLPROC usedButOptimizedOut
@fif_codegen_avoid usedButOptimizedOut CALLDICT
*/
