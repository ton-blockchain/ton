fun moddiv2(x: int, y: int): (int, int) builtin;

/**
@compilation_should_fail
@stderr
"""
`builtin` used for non-builtin function
fun moddiv2
"""
*/
