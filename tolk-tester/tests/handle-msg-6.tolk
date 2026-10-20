
@method_id(101)
fun test1() {
    return 0;
}

fun onBouncedMessage(in: InMessageBounced) {
    __expect_inline(true);
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

@fif_codegen_avoid DECLPROC onBouncedMessage()

@fif_codegen
"""
  onInternalMessage() PROC:<{
    DROP
    INMSG_BOUNCED
    IFJMP:<{
      INMSG_VALUEEXTRA
      DICTEMPTY
      IFJMP:<{
        INMSG_FWDFEE
"""
  */
