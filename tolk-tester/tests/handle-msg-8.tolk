
@method_id(101)
fun test1() {
    return 0;
}

@on_bounced_policy("manual")
fun onInternalMessage(in: InMessage) {
    // test that chaining for a mutable `in` field works (in codegen)
    in.body.skipBits(1).skipBits(1);
    return in.body.loadUint(2);
}

/**
@testcase | 101 | | 0

@fif_codegen
"""
  onInternalMessage() PROC:<{
    2 LDU
    NIP                     	//  in.body
    2 PLDU                  	//  '7
  }>
"""
*/
