const int10:int=10;

fun just10(): int { return int10; }
fun eq(v: int): int { return`v` }

@method_id(101,) fun `get_-1`  (): int {return-1}
@method_id(102,) fun `get_--1` (): int {return--1}
@method_id(103,) fun `get_---1`(): int {return---1}
@method_id(104,) fun `get_+++1`(): int {return+++1}
@method_id(105,) fun `get_+-+1`(): int {return+-+1}

global `some()var`:int;

@method_id(110) fun `some_math`(): int {
    `some()var`=--6;
    return 1*-2*-3*-4*just10()*-5+-`some()var`+--`some()var`---`some()var`;
}

@method_id(111) fun `negative_nums`(a:int):int {
    var m$0:int=1;
    var m1:int=-(+0x1)*m$0;
    return `a`*-1*-(1)*---(1)*+just10()+-`just10`()*m1*-m1+-eq(m1)----0x1;
}

@method_id(112) fun `bitwise~ops`(flags:int):[bool,bool] {
    return[
        (just10()-3==just10()-(4)--1)|((2==2)&(eq(eq(10)) -3==just10()--13)),
        ((flags&0xFF)!=0)
    ]
}

@method_id(113)fun`unary+bitwise-constant`():[int,int,int]{
    return [~-~~+-3, ~+3-~9, -(-~+-20-~ 10+3+~38&39)];
}

@method_id(114)fun`unary+bitwize-parametrized`(c3:int, c9:int, c20:int, c10:int, c38:int):[int,int,int]{
    return [~-~~+-c3, ~+c3-~`c9`, -(-~+-c20-~c10+c3+~c38&39)];
}

fun add3(a: int, b: int, c: int) { return a+b+c; }

@method_id(115) fun unary_const_check(): [int,int] {
    var fst1: int=-1;
    var snd1: int=-1;
    var trd1: int=+2;
    var (fst2,snd2,trd2)=(-1,-1,+2);
    return [add3(fst2,snd2,trd2),add3(fst1,snd1,trd1)];
}

fun slice.`load:u32`(mutate self): int {
    return self.loadUint(32)
}

@method_id(116) fun `call_~_via_backticks`():[int,int,int,int] {
    var b:builder = beginCell().storeUint(1, 32).storeUint(2, 32).storeUint(3, 32).storeUint(4, 32);
    var `cs`:slice = b.endCell().beginParse();
    val one:int=`cs`.`loadUint`(32);
    val (two:int,three:int) = (`cs`.`loadUint`(32), cs.`load:u32`());
    val four:int = cs.`load:u32`();
    return [one,two,three,four];
}

fun`main`(){}

/**
    method_id  | in | out
@testcase | 101 |    | -1
@testcase | 102 |    | 1
@testcase | 103 |    | -1
@testcase | 104 |    | 1
@testcase | 105 |    | -1
@testcase | 110 |    | 1194
@testcase | 111 | -1 | 22
@testcase | 112 | 0  | [ -1 0 ]
@testcase | 113 |    | [ -4 6 -4 ]
@testcase | 114 | 3 9 20 10 38 | [ -4 6 -4 ]
@testcase | 115 |    | [ 0 0 ]
@testcase | 116 |    | [ 1 2 3 4 ]

@fif_codegen
"""
  get_+-+1 PROC:<{
    -1 PUSHINT
  }>
"""

@fif_codegen
"""
  unary+bitwise-constant PROC:<{
    -4 PUSHINT
    6 PUSHINT
    -4 PUSHINT
    TRIPLE
  }>
"""

@fif_codegen
"""
  unary_const_check PROC:<{
    -1 PUSHINT	//  fst1=-1
    DUP	//  fst1=-1 snd1=-1
    2 PUSHINT	//  fst1=-1 snd1=-1 trd1=2
    s1 s1 s0 PUSH3	//  fst1=-1 snd1=-1 trd1=2 fst2=-1 snd2=-1 trd2=2
    add3 CALLDICT	//  fst1=-1 snd1=-1 trd1=2 '13
    3 -ROLL	//  '13 fst1=-1 snd1=-1 trd1=2
    add3 CALLDICT	//  '13 '14
    PAIR	//  '12
  }>
"""

 */

