fun main(s: int) {
  var (z, t) = (17, s);
  while (z > 0) {
    t = s;
    z -= 1;
  }
  return ~ t;
}

/**
   method_id | in | out
@testcase    | 0  | 1  | -2
@testcase    | 0  | 5  | -6
*/
