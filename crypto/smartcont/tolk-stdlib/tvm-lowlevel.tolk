// A part of standard library for Tolk
tolk 1.3

/// Usually `c3` has a continuation initialized by the whole code of the contract. It is used for function calls.
/// The primitive returns the current value of `c3`.
@pure
fun getTvmRegisterC3(): continuation
    asm "c3 PUSH"

/// Updates the current value of `c3`. Usually, it is used for updating smart contract code in run-time.
/// Note that after execution of this primitive the current code
/// (and the stack of recursive function calls) won't change,
/// but any other function call will use a function from the new code.
fun setTvmRegisterC3(c: continuation): void
    asm "c3 POP"

/// Transforms a slice into a simple ordinary continuation, with `c.code = s` and an empty stack and savelist.
@pure
fun transformSliceToContinuation(s: slice): continuation
    asm "BLESS"

/// Moves a variable to the top of the stack.
@pure
fun T.stackMoveToTop(mutate self): void
    asm "NOP"


/**
    Standard TVM exit codes.
    [See docs](https://docs.ton.org/tvm/exit-codes)
 */

/// Standard exit codes of the compute and action phases, reserved by TVM.
/// Exit codes 0 and 1 indicate normal (successful) execution of the compute phase.
/// Exit code 0 indicates normal (successful) execution of the action phase.
/// TON Blockchain reserves exit code values from 0 to 127.
/// For custom errors, use numbers from 128 to 1024 (above is not recommended due to extra asm instructions).
enum VmExitCode {
    NormalTermination = 0           // successful execution of compute or action phase
    AlternativeTermination = 1      // alternative success code; reserved, but never occurs
    StackUnderflow = 2              // not enough stack values for the instruction
    StackOverflow = 3               // too many elements copied into a closure continuation
    IntegerOverflow = 4             // integer does not fit into −2^256 .. 2^256−1, or division by zero
    IntegerOutOfRange = 5           // integer is out of expected range (range check error)
    InvalidOpcode = 6               // instruction or its fift mapping not found
    TypeCheckError = 7              // argument to a primitive is of incorrect value type
    CellOverflow = 8                // writing too much data or refs to a builder
    CellUnderflow = 9               // reading more data or refs than a slice contains
    DictionaryError = 10            // dictionary (hashmap) manipulation error
    UnknownError = 11               // unknown error, may be thrown by user programs
    FatalError = 12                 // thrown by TVM in situations deemed impossible
    OutOfGasError = 13              // not enough gas to finish execution (shown as -14 at runtime)
    VirtualizationError = 14        // reserved, but never thrown
    ActionListInvalid = 32          // action list contains exotic or unparseable cells
    ActionListTooLong = 33          // more than 255 actions queued
    ActionInvalid = 34              // action is invalid or not supported
    InvalidSourceAddress = 35       // invalid source address in outbound message
    InvalidDestAddress = 36         // invalid destination address in outbound message
    NotEnoughToncoin = 37           // not enough Toncoin to send or pay fees
    NotEnoughExtraCurrencies = 38   // not enough extra currencies to send the specified amount
    MessageDoesNotFit = 39          // outbound message does not fit into a cell after rewriting
    CannotProcessMessage = 40       // not enough funds, message too large, or Merkle depth too big
    LibraryReferenceNull = 41       // library reference is null during library change action
    LibraryChangeError = 42         // error during library change action
    LibraryLimitsExceeded = 43      // too many library cells or Merkle tree depth exceeded
    AccountStateSizeExceeded = 50   // account state exceeded limits (config param 43)
    OpcodeDoesNotMatch = 63         // default opcode mismatch for [T.fromCell] and [T.fromSlice]
}
