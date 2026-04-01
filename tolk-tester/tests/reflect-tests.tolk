import "@stdlib/reflection"
import "@stdlib/strings"

const CONST_LINE = reflect.sourceLocation().lineNo      // 4
const CONST_CHAR = reflect.sourceLocation().charNo      // 20 (word `reflect`)
const CONST_FILE = reflect.sourceLocation().fileName    // full realpath

const ALL_LOC = reflect.sourceLocation()        // { 8, 17 }
const LINE_OF_ALL_LOC = ALL_LOC.lineNo          // 8

global tLog: tuple;

fun getLineOfCaller(loc: SourceLocation = reflect.sourceLocation()): int {
    return loc.lineNo;
}

fun logInner(x: int, loc: SourceLocation = reflect.sourceLocation()) {
    tLog.push([x, loc.lineNo]);
}

fun logOuter(x: int, loc: SourceLocation = reflect.sourceLocation()) {
    tLog.push(x);
    logInner(x, loc);  // pass-through the same loc
}

@method_id(101)
fun locOfConst() {
    return (CONST_LINE, CONST_CHAR, CONST_FILE.equalTo(reflect.sourceLocation().fileName), ALL_LOC.charNo, LINE_OF_ALL_LOC);
}

@method_id(102)
fun locOfDirectCall() {
    val loc = reflect.sourceLocation();
    return (
        loc.lineNo,
        loc.lineNo,
        reflect.sourceLocation().lineNo,
    );
}

@method_id(103)
fun defaultParam1() {
    var line1 = getLineOfCaller();
    _ = 0;
    var line2 = getLineOfCaller();  // = line1 + 2
    return line2 - line1;
}

@method_id(104)
fun charNo() {
    var locs = [reflect.sourceLocation(), reflect.sourceLocation()];
    return (locs.first().lineNo == locs.last().lineNo, locs.first().charNo, locs.last().charNo);
}

@method_id(105)
fun passThrough(): array<unknown> {
    tLog = [];
    logOuter(123);
    logInner(8);
    logOuter(456);
    return tLog;
}

struct WithLoc {
    f1: SourceLocation = reflect.sourceLocation()
    f2: SourceLocation = reflect.sourceLocation()
    f3: array<SourceLocation> = [reflect.sourceLocation()]
}

@method_id(106)
fun fieldsWithLoc() {
    var wl = WithLoc {
        f2: reflect.sourceLocation()
        // f1 and f3 will have lineNo and others from fields default (their location)
    };
    return (wl.f1.lineNo, wl.f2.lineNo, wl.f3.first().lineNo)
}

const TO_STRING = reflect.sourceLocationAsString();      // "fileName:lineNo:charNo"

fun logLocStr(locStr: string = reflect.sourceLocationAsString()) {
    return locStr;
}

@method_id(107)
fun asStringMethod() {
    val s1 = logLocStr();       // "/full/path/reflect-tests.tolk:87:5"
    val s2 = logLocStr();       // "/full/path/reflect-tests.tolk:88:5"
    return (
        s1.equalTo(s2),
        TO_STRING.equalTo(s1),
        s2.calculateLength() > 10,
    );
}

struct WithOptSlice {
    x: slice?
    nn: int100
}

@method_id(110)
fun estimateSerialization() {
    return (
        reflect.estimateSerializationOf<slice?>(),
        reflect.estimateSerializationOf<WithOptSlice>(),
    )
}

fun main() {
    __expect_type(reflect.sourceLocation(), "SourceLocation");
}

/**
@testcase | 101 | | 4 20 -1 17 8
@testcase | 102 | | 33 33 37
@testcase | 103 | | 2
@testcase | 104 | | -1 17 43
@testcase | 105 | | [ 123 [ 123 58 ] [ 8 59 ] 456 [ 456 60 ] ]
@testcase | 106 | | 65 73 67
@testcase | 107 | | 0 0 -1
@testcase | 110 | | 1 9999 0 4 101 9999 0 4
 */
