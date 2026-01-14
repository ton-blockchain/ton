
global gTens: (int, int);

@pure
fun set0_pure() {
    (gTens!).0 = 888;
}

/**
@compilation_should_fail
@stderr modifying a global in a pure function
 */
