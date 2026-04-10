struct A { 
    private readonly a: () 
    readonly b: () 
    private private e: () 
}

/**
@compilation_should_fail
@stderr expected field name, got `private`
@stderr private private e
 */
