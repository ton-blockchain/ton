fun main(x: int): int {
  var i: int = 0;
  // int f = false;
  do {
    i = i + 1;
    if (i > 5) {
      return 1;
    }
    var f: bool = (i * i == 64);
  } while (!f);
  return -1;
}

/**
    method_id | in | out
@testcase | 0  | 0  | 1

@code_hash 36599880583276393028571473830850694081778552118303309411432666239740650614479
*/
