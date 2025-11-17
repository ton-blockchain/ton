struct TryHackCompilerByNever {
    a: int8
    value: never
}

@noinline
fun TryHackCompilerByNever.getNever(self): never {
    return self.value
}

fun main() {
    val c = TryHackCompilerByNever { a: 100 };
    val nev: never = c.getNever();
    throw 24;
}

/**
@compilation_should_fail
@stderr field `value` missed in initialization of struct `TryHackCompilerByNever` 
 */
