fun main() {
  var a = 1;
  (a += 1) += 2;
}

/**
@compilation_should_fail
@stderr can not assign to a temporary expression
*/
