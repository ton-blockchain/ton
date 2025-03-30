fun main() {
  var a = 1;
  (a += 1) += 2;
}

/**
@compilation_should_fail
@stderr assignment can not be used as lvalue
*/
