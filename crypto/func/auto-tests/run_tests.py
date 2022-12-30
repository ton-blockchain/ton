import os
import os.path
import subprocess
import sys
import tempfile

def getenv(name, default=None):
    if name in os.environ:
        return os.environ[name]
    if default is None:
        print("Environment variable", name, "is not set", file=sys.stderr)
        exit(1)
    return default

FUNC_EXECUTABLE = getenv("FUNC_EXECUTABLE", "func")
FIFT_EXECUTABLE = getenv("FIFT_EXECUTABLE", "fift")
#FUNC_STDLIB = getenv("FUNC_STDLIB")
FIFT_LIBS = getenv("FIFT_LIBS")
TMP_DIR = tempfile.mkdtemp()
COMPILED_FIF = os.path.join(TMP_DIR, "compiled.fif")
RUNNER_FIF = os.path.join(TMP_DIR, "runner.fif")

if len(sys.argv) != 2:
    print("Usage : run_tests.py tests_dir", file=sys.stderr)
    exit(1)
TESTS_DIR = sys.argv[1]

class ExecutionError(Exception):
    pass

def compile_func(f):
    res = subprocess.run([FUNC_EXECUTABLE, "-o", COMPILED_FIF, "-SPA", f], capture_output=True, timeout=10)
    if res.returncode != 0:
        raise ExecutionError(str(res.stderr, "utf-8"))

def run_runner():
    res = subprocess.run([FIFT_EXECUTABLE, "-I", FIFT_LIBS, RUNNER_FIF], capture_output=True, timeout=10)
    if res.returncode != 0:
        raise ExecutionError(str(res.stderr, "utf-8"))
    s = str(res.stdout, "utf-8")
    s = [x.strip() for x in s.split("\n")]
    return [x for x in s if x != ""]

tests = [s for s in os.listdir(TESTS_DIR) if s.endswith(".fc")]
tests.sort()
print("Found", len(tests), "tests", file=sys.stderr)
for ti, tf in enumerate(tests):
    print("Running test %d/%d: %s" % (ti + 1, len(tests), tf), file=sys.stderr)
    tf = os.path.join(TESTS_DIR, tf)
    try:
        compile_func(tf)
    except ExecutionError as e:
        print(file=sys.stderr)
        print("Compilation error", file=sys.stderr)
        print(e, file=sys.stderr)
        exit(2)
    with open(tf, "r") as fd:
        lines = fd.readlines()
    cases = []
    for s in lines:
        s = [x.strip() for x in s.split("|")]
        if len(s) == 4 and s[0].strip() == "TESTCASE":
            cases.append(s[1:])
    if len(cases) == 0:
        print(file=sys.stderr)
        print("Error: no test cases", file=sys.stderr)
        exit(2)

    with open(RUNNER_FIF, "w") as f:
        print("\"%s\" include <s constant code" % COMPILED_FIF, file=f)
        for function, test_in, _ in cases:
            print(test_in, function, "code 1 runvmx abort\"exitcode is not 0\" .s cr { drop } depth 1- times", file=f)
    try:
        func_out = run_runner()
        if len(func_out) != len(cases):
            raise ExecutionError("Unexpected number of lines")
        for i in range(len(func_out)):
            if func_out[i] != cases[i][2]:
                raise ExecutionError("Error on case %d: expected '%s', found '%s'" % (i + 1, cases[i][2], func_out[i]))
    except ExecutionError as e:
        print(file=sys.stderr)
        print("Error:", file=sys.stderr)
        print(e, file=sys.stderr)
        print(file=sys.stderr)
        print("Compiled:", file=sys.stderr)
        with open(COMPILED_FIF, "r") as f:
            print(f.read(), file=sys.stderr)
        exit(2)
    print("  OK, %d cases" % len(cases), file=sys.stderr)

print("Done", file=sys.stderr)
