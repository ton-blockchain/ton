
@method_id(101)
fun test1() {
    return 0;
}

fun onBouncedMessage(in: InMessageBounced) {
    if (!in.valueExtra.isEmpty()) {
        return;
    }
    throw in.originalForwardFee;
}

fun onInternalMessage(in: InMessage) {
    return in.senderAddress;
}

/**
@testcase | 101 | | 0

@fif_codegen
"""
  onBouncedMessage() PROC:<{
    DROP
    INMSG_VALUEEXTRA
    DICTEMPTY
    IFNOTJMP:<{
    }>
    INMSG_FWDFEE
    0 PUSHINT
    GETORIGINALFWDFEE
    THROWANY
  }>
"""

@fif_codegen
"""
  onInternalMessage() PROC:<{
    INMSG_BOUNCED
    IFJMP:<{
      onBouncedMessage() CALLDICT
    }>
    DROP
    INMSG_SRC
  }>
"""
  */
