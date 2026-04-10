
@method_id(101)
fun test1() {
    return 0;
}

fun onBouncedMessage(in: InMessageBounced) {
    in.bouncedBody.skipBouncedPrefix();
    throw in.bouncedBody.loadUint(32);
}

fun onInternalMessage(in: InMessage) {
    return in.body;
}

/**
@testcase | 101 | | 0

@fif_codegen
"""
  onInternalMessage() PROC:<{
    INMSG_BOUNCED
    IFJMP:<{
       32 LDU
       NIP
       32 PLDU
       THROWANY
    }>                      	//  in.body
  }>
"""
*/