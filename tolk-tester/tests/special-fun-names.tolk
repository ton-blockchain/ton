fun onInternalMessage() { return 0; }
fun onExternalMessage() { return -1; }
fun onRunTickTock() { return -2; }
fun onSplitPrepare() { return -3; }
fun onSplitInstall() { return -4; }

// 'execute' and many other names are reserved words in Fift,
// but since they are codegenerated like `execute()` (it's a valid Fift name!), no collisions
@noinline
fun execute() { return 123 }
@noinline
fun `PUSHINT`() { return 456 }
@noinline
fun `@havebits`() { return 789 }
@noinline
fun `f with spaces`() { return `g with spaces` }

global swap: int;
global `g with spaces`: int;

@method_id(101)
fun test101() {
    swap = 1;
    `g with spaces` = 10;
    return execute() + `PUSHINT`() + `@havebits`() + swap + `f with spaces`();
}

/**
@testcase | 0  | | 0
@testcase | -1 | | -1
@testcase | -2 | | -2
@testcase | -3 | | -3
@testcase | -4 | | -4
@testcase | 101 | | 1379

@fif_codegen
"""
  0 DECLMETHOD onInternalMessage()
  -1 DECLMETHOD onExternalMessage()
  -2 DECLMETHOD onRunTickTock()
  -3 DECLMETHOD onSplitPrepare()
  -4 DECLMETHOD onSplitInstall()
"""

@fif_codegen DECLPROC execute()
@fif_codegen execute() CALLDICT
@fif_codegen DECLPROC f-with-spaces()
@fif_codegen DECLGLOBVAR $swap
@fif_codegen DECLGLOBVAR $g-with-spaces
 */
