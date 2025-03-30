fun main() {
  var x = 1;
  x += (var y = 2);
}

/**
@compilation_should_fail
@stderr expected <expression>, got `var`
*/
