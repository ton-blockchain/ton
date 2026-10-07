global g1: int;
global g2: int;
global g3: int;
global g4: int;
global g5: int;
global g6: int;
global g7: int;
global g8: int;
global g9: int;
global g10: int;
global g11: int;
global g12: int;
global g13: int;
global g14: int;
global g15: int;
global g16: int;
global g17: int;
global g18: int;
global g19: int;
global g20: int;
global g21: int;
global g22: int;
global g23: int;
global g24: int;
global g25: int;
global g26: int;
global g27: int;
global g28: int;
global g29: int;
global g30: int;
global g31: int;
global g32: int;

fun main(): int {
    g1 = 1;
    g2 = 2;
    g3 = 3;
    g4 = 4;
    g5 = 5;
    g6 = 6;
    g7 = 7;
    g8 = 8;
    g9 = 9;
    g10 = 10;
    g11 = 11;
    g12 = 12;
    g13 = 13;
    g14 = 14;
    g15 = 15;
    g16 = 16;
    g17 = 17;
    g18 = 18;
    g19 = 19;
    g20 = 20;
    g21 = 21;
    g22 = 22;
    g23 = 23;
    g24 = 24;
    g25 = 25;
    g26 = 26;
    g27 = 27;
    g28 = 28;
    g29 = 29;
    g30 = 30;
    g31 = 31;
    g32 = 32;

    return g1 + g2 + g3 + g4 + g5 + g6 + g7 + g8
         + g9 + g10 + g11 + g12 + g13 + g14 + g15 + g16
         + g17 + g18 + g19 + g20 + g21 + g22 + g23 + g24
         + g25 + g26 + g27 + g28 + g29 + g30 + g31 + g32;
}

/**
@compilation_should_fail
@stderr too many global variables (more than 31)
@stderr g32
 */
