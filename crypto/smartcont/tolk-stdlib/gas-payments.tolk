// A part of standard library for Tolk
tolk 0.12

/**
  Gas and payment related primitives.
 */

/// Returns amount of gas (in gas units) consumed in current Computation Phase.
fun getGasConsumedAtTheMoment(): coins
    asm "GASCONSUMED";

/// This function is required to be called when you process an external message (from an outer world)
/// and "accept" it to blockchain.
/// Without calling this function, an external message would be discarded.
/// As an effect, the current smart contract agrees to buy some gas to finish the current transaction.
/// For more details, check [accept_message effects](https://ton.org/docs/#/smart-contracts/accept).
fun acceptExternalMessage(): void
    asm "ACCEPT";

/// When processing an internal message, by default, the limit of gas consumption is determined by incoming message.
/// Functions [setGasLimit] and [setGasLimitToMaximum] allow you to change this behavior.
/// Sets current gas limit `gl` to its maximal allowed value `gm`, and resets the gas credit `gc` to zero,
/// decreasing the value of `gr` by `gc` in the process.
fun setGasLimitToMaximum(): void
    asm "ACCEPT";

/// When processing an internal message, by default, the limit of gas consumption is determined by incoming message.
/// Functions [setGasLimit] and [setGasLimitToMaximum] allow you to change this behavior.
/// Sets current gas limit `gl` to the minimum of limit and `gm`, and resets the gas credit `gc` to zero.
/// If the gas consumed so far (including the present instruction) exceeds the resulting value of `gl`,
/// an (unhandled) out of gas exception is thrown before setting new gas limits.
fun setGasLimit(limit: int): void
    asm "SETGASLIMIT";

/// Calculates fee (amount in nanotoncoins to be paid) for a transaction which consumed [gasUsed] gas units.
fun calculateGasFee(workchain: int8, gasUsed: int): coins
    asm(gasUsed workchain) "GETGASFEE";

/// Same as [calculateGasFee], but without flat price (you have supposed to read https://docs.ton.org/develop/howto/fees-low-level)
fun calculateGasFeeWithoutFlatPrice(workchain: int8, gasUsed: coins): coins
    asm(gasUsed workchain) "GETGASFEESIMPLE";

/// Calculates amount of nanotoncoins you should pay for storing a contract of provided size for [seconds].
/// [bits] and [cells] represent contract state (code + data).
fun calculateStorageFee(workchain: int8, seconds: int, bits: int, cells: int): coins
    asm(cells bits seconds workchain) "GETSTORAGEFEE";

/// Calculates amount of nanotoncoins you should pay to send a message of specified size.
fun calculateMessageFee(workchain: int8, bits: int, cells: int): coins
    asm(cells bits workchain) "GETFORWARDFEE";

/// Same as [calculateMessageFee], but without lump price (you have supposed to read https://docs.ton.org/develop/howto/fees-low-level)
fun calculateMessageFeeWithoutLumpPrice(workchain: int8, bits: int, cells: int): coins
    asm(cells bits workchain) "GETFORWARDFEESIMPLE";

/// Calculates fee that was paid by the sender of an incoming internal message.
fun calculateOriginalMessageFee(workchain: int8, incomingFwdFee: coins): coins
    asm(incomingFwdFee workchain) "GETORIGINALFWDFEE";

/// Returns the amount of nanotoncoins current contract debts for storage. ("due" and "debt" are synonyms)
/// If it has no debt, `0` is returned.
fun contract.getStorageDuePayment(): coins
    asm "DUEPAYMENT";

/// Returns the amount of nanotoncoins charged for storage.
/// (during storage phase preceeding to current computation phase)
@pure
fun contract.getStoragePaidPayment(): coins
    asm "STORAGEFEES";
