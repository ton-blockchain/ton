fun main() {
    if ((1==1)|(2==2)&(3==3)) {
    }
}

/**
@compilation_should_fail
@stderr mixing | with & without parenthesis
*/
