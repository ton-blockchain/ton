
@method_id(101)
fun test1() {
    return 0;
}

@noinline
fun onBouncedMessage(in: InMessageBounced) {
    throw in.valueCoins;
}

fun onInternalMessage(in: InMessage) {
    return in.valueCoins;
}

/**
@testcase | 101 | | 0

@fif_codegen
"""
  onBouncedMessage() PROC:<{
    DROP
    INMSG_VALUE
    THROWANY
  }>
"""

@fif_codegen
"""
  onInternalMessage() PROC:<{   //  in.body
    INMSG_BOUNCED               //  in.body '1
    IFJMP:<{                    //  in.body
      onBouncedMessage() CALLDICT       //
    }>                          //  in.body
    DROP                        //
    INMSG_VALUE                 //  '3
  }>
"""
  */
