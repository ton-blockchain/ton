struct Wrapper<T> {
    value: T;
}

fun main() {
    var c1: Wrapper< Wrapper<(int,int)> > = {value:{value:(1,2)}};              // ok
    var c2: Wrapper< Wrapper<(int?,int?)> > = {value:{value:(null,null)}};      // ok

    var c3: Wrapper< Wrapper<(int,int)> > = {value:{value:0}};
}

/**
@compilation_should_fail
@stderr can not assign `int` to field of type `(int, int)`
@stderr {value:0}
 */
