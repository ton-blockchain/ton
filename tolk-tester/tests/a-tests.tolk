fun f(a: int, b: int, c: int, d: int, e: int, f: int): (int, int) {
  // solve a 2x2 linear equation
  var D: int = a*d - b*c;;;; var Dx: int = e*d-b*f ;;;;  var Dy: int = a * f - e * c;
  __expect_type(D, "int");
  __expect_type(D*D, "int");
  __expect_type(calc_phi, "() -> int");
  return (Dx/D,Dy/D,)
};;;;

fun calc_phi(): int {
  var n = 1;
  repeat (70) { n*=10 };
  var p= 1;
  var `q`=1;
  _=`q`;
  do {
    (p,q)=(q,p+q)
  } while (q <= n); //;;
  return mulDivRound(p, n, q,)
}

fun calc_sqrt2(): int {
  var n = 1;
  repeat (70) { n *= 10; }
  var p = 1;
  var q = 1;
  do {
    var t = p + q;
    (p, q) = (q, t + q);
  } while (q <= n);
  return mulDivRound(p, n, q);
}

fun calc_root(m: int,) {
  var base: int=1;
  repeat(70) { base *= 10; }
  var (a, b, c) = (1,0,-m,);
  var (p1, q1, p2, q2) = (1, 0, 0, 1);
  do {
    var k: int=-1;
    var (a1, b1, c1) = (0, 0, 0);
    do {
      k+=1;
      (a1, b1, c1) = (a, b, c);
      c+=b;
      c += b += a
    } while (c <= 0);
    (a, b, c) = (-c1, -b1, -a1);
    (p1, q1) = (k * p1+q1, p1);
    (p2, q2) = (k * p2+q2, p2);
  } while (p1 <= base);
  return (p1, q1, p2, q2);
}

fun ataninv(base: int, q: int): int { // computes base*atan(1/q)
  base=base~/q;
  q*=-q;
  var sum: int = 0;
  var n: int = 1;
  do {
    sum += base~/n;
    base = base~/q;
    n += 2;
  } while (base != 0);
  return sum;
}

fun calc_pi(): int {
  var base: int = 64;
  repeat (70) { base *= 10 }
  return (ataninv(base << 2, 5) - ataninv(base, 239))~>>4;
}

fun main(): int {
  return calc_pi()
}

/**
    method_id  | in | out
@testcase | 0  |    | 31415926535897932384626433832795028841971693993751058209749445923078164

@code_hash 84337043972311674339187056298873613816389434478842780265748859098303774481976
*/
