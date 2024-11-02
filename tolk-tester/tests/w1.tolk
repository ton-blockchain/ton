fun main(id: int): (int, int) {
  if (id > 0) {
    if (id > 10) {
      return (2 * id, 3 * id);
    }
  }
  return (5, 6);
}
/**
    method_id | in | out
@testcase | 0  | 0  | 5 6
@testcase | 0  | 4  | 5 6
@testcase | 0  | 11 | 22 33
*/
