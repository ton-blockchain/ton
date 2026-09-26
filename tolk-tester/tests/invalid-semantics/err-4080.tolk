fun int.methodWith1Param(self, param: int) {

}

fun main() {
    val x = 10;
    x.methodWith1Param(2, "asdf");
}

/**
@compilation_should_fail
@stderr too many arguments in call to `int.methodWith1Param`, expected 1, have 2
@stderr note: function declared here
@stderr err-4080.tolk:1
 */
