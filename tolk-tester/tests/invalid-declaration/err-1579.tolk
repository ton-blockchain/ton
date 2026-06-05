type P2 = (int, int)
type P4 = (P2, P2)
type P8 = (P4, P4)
type P16 = (P8, P8)
type P32 = (P16, P16)
type P64 = (P32, P32)
type P128 = (P64, P64)
type P256 = (P128, P128)

fun main(flag: bool, t: P256): int {
    return t.1.1.1.1.1.1.1.1;
}

/**
@compilation_should_fail
@stderr generated TVM stack is too deep while compiling function `main`
@stderr TVM can not store more than 255 elements on the stack
@stderr hint: try splitting very wide tensors/structs, storing parts in cells/tuples
@stderr in function `main`
@stderr              ^^^^
 */
