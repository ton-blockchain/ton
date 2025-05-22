@method_id(101)
fun test1(): int {
    var x: int = false as int;
    if (x == true as int) {
        x= 100500;
    }
    return x;
}

fun main(s: int) {
    var (z, t) = (17, s);
    while (z > 0) {
        t = s;
        z -= 1;
    }
    return ~ t;
}

/**
    method_id | in | out
@testcase | 0   | 1  | -2
@testcase | 0   | 5  | -6
@testcase | 101 |    | 0

Below, I just give examples of @fif_codegen tag:
* a pattern can be single-line (after the tag), or multi-line, surrounded with """
* there may be multiple @fif_codegen, they all will be checked
* identation (spaces) is not checked intentionally
* "..." means any number of any lines
* lines not divided with "..." are expected to be consecutive in fif output
* //comments can be omitted, but if present, they are also expected to be equal
* there is also a tag @fif_codegen_avoid to check a pattern does not occur

@fif_codegen
"""
main PROC:<{     // s
  17 PUSHINT     //  s '3=17
  OVER           //  s z=17 t
  WHILE:<{
    ...
  }>DO<{ //  s z t
    ...
    s1 s(-1) PUXC //  s t z
    ...
  2 1 BLKDROP2
  ...
}>
"""

@fif_codegen
"""
main PROC:<{
  ...
  WHILE:<{
    ...
           }>DO<{
  ...
    }>
        }END>c
"""

@fif_codegen
"""
    OVER
    0 GTINT   //  s z t '5
"""

@fif_codegen
"""
  "Asm.fif" include
  ...
  PROGRAM{
  ...
  }END>c
"""

@fif_codegen
"""
test1 PROC:<{
FALSE
}>
"""

@fif_codegen NOT //  '8
@fif_codegen main PROC:<{

@fif_codegen_avoid PROCINLINE
@fif_codegen_avoid END c
@fif_codegen_avoid
"""
multiline
can also be
"""
*/
