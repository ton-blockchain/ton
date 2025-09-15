
@method_id(101)
fun test1() {
    return 0;
}

@inline_ref
fun onBouncedMessage(in: InMessageBounced) {
    contract.setData(createEmptyCell());
}

fun onInternalMessage(in: InMessage) {
    return 123;
}

/**
@testcase | 101 | | 0

@fif_codegen
"""
  onBouncedMessage() PROCREF:<{	//  in.body
    DROP
    <b b> PUSHREF
    c4 POP
  }>
"""

@fif_codegen
"""
  onInternalMessage() PROC:<{   //  in.body
    INMSG_BOUNCED               //  in.body '1
    IFJMP:<{                    //  in.body
      onBouncedMessage() INLINECALLDICT
    }>
    DROP
    123 PUSHINT
  }>
"""

 */
