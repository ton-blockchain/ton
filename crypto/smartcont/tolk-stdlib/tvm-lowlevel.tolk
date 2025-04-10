// A part of standard library for Tolk
tolk 0.12

/// Usually `c3` has a continuation initialized by the whole code of the contract. It is used for function calls.
/// The primitive returns the current value of `c3`.
@pure
fun getTvmRegisterC3(): continuation
    asm "c3 PUSH";

/// Updates the current value of `c3`. Usually, it is used for updating smart contract code in run-time.
/// Note that after execution of this primitive the current code
/// (and the stack of recursive function calls) won't change,
/// but any other function call will use a function from the new code.
fun setTvmRegisterC3(c: continuation): void
    asm "c3 POP";

/// Transforms a `slice` [s] into a simple ordinary continuation `c`, with `c.code = s` and an empty stack and savelist.
@pure
fun transformSliceToContinuation(s: slice): continuation
    asm "BLESS";

/// Moves a variable or a value [x] to the top of the stack.
@pure
fun T.stackMoveToTop(mutate self): void
    asm "NOP";
