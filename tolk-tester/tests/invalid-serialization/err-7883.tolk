struct TryHackCompilerByNever {
    a: int8
    value: never
}

@noinline
fun TryHackCompilerByNever.getNever(self): never {
    return self.value
}

fun main() {
    val c = TryHackCompilerByNever.fromSlice(stringHexToSlice("0102"));
    val nev: never = c.getNever();
    throw 24;
}

/**
@compilation_should_fail
@stderr because field `TryHackCompilerByNever.value` of type `never` can't be serialized
@stderr because type `never` is not serializable  
 */
