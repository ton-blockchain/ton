fun onInternalMessage() { return 0; }
fun onExternalMessage() { return -1; }
fun onRunTickTock() { return -2; }
fun onSplitPrepare() { return -3; }
fun onSplitInstall() { return -4; }

/**
@experimental_options remove-unused-functions

@testcase | 0  | | 0
@testcase | -1 | | -1
@testcase | -2 | | -2
@testcase | -3 | | -3
@testcase | -4 | | -4

@fif_codegen
"""
  0 DECLMETHOD onInternalMessage
  -1 DECLMETHOD onExternalMessage
  -2 DECLMETHOD onRunTickTock
  -3 DECLMETHOD onSplitPrepare
  -4 DECLMETHOD onSplitInstall
"""
 */
