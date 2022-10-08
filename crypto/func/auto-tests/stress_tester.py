import os
import os.path
import random
import subprocess
import sys
import tempfile

def getenv(name, default=None):
    if name in os.environ:
        return os.environ[name]
    if default is not None:
        return default
    print("Environemnt variable", name, "is not set", file=sys.stderr)
    exit(1)

VAR_CNT = 5
TMP_DIR = tempfile.mkdtemp()
FUNC_EXECUTABLE = getenv("FUNC_EXECUTABLE", "func")
FIFT_EXECUTABLE = getenv("FIFT_EXECUTABLE", "fift")
FIFT_LIBS = getenv("FIFT_LIBS")
MAGIC = 123456789

var_idx = 0
def gen_var_name():
    global var_idx
    var_idx += 1
    return "i%d" % var_idx

class State:
    def __init__(self, x):
        self.x = x
        self.vs = [0] * VAR_CNT

class Code:
    pass

class CodeEmpty(Code):
    def execute(self, state):
        return None

    def write(self, f, indent=0):
        pass

class CodeReturn(Code):
    def __init__(self, value):
        self.value = value

    def execute(self, state):
        return [self.value] + state.vs

    def write(self, f, indent=0):
        print("  " * indent + "return (%d, %s);" % (self.value, ", ".join("v%d" % i for i in range(VAR_CNT))), file=f)

class CodeAdd(Code):
    def __init__(self, i, value):
        self.i = i
        self.value = value

    def execute(self, state):
        state.vs[self.i] += self.value
        return None

    def write(self, f, indent=0):
        print("  " * indent + "v%d += %d;" % (self.i, self.value), file=f)

class CodeBlock(Code):
    def __init__(self, code):
        self.code = code

    def execute(self, state):
        for c in self.code:
            res = c.execute(state)
            if res is not None:
                return res
        return None

    def write(self, f, indent=0):
        for c in self.code:
            c.write(f, indent)

class CodeIfRange(Code):
    def __init__(self, l, r, c1, c2):
        self.l = l
        self.r = r
        self.c1 = c1
        self.c2 = c2

    def execute(self, state):
        if self.l <= state.x < self.r:
            return self.c1.execute(state)
        else:
            return self.c2.execute(state)

    def write(self, f, indent=0):
        print("  " * indent + "if (in(x, %d, %d)) {" % (self.l, self.r), file=f)
        self.c1.write(f, indent + 1)
        if isinstance(self.c2, CodeEmpty):
            print("  " * indent + "}", file=f)
        else:
            print("  " * indent + "} else {", file=f)
            self.c2.write(f, indent + 1)
            print("  " * indent + "}", file=f)

class CodeRepeat(Code):
    def __init__(self, n, c, loop_type):
        if loop_type == 2:
            n = max(n, 1)
        self.n = n
        self.c = c
        self.loop_type = loop_type

    def execute(self, state):
        for _ in range(self.n):
            res = self.c.execute(state)
            if res is not None:
                return res
        return None

    def write(self, f, indent=0):
        if self.loop_type == 0:
            print("  " * indent + "repeat (%d) {" % self.n, file=f)
            self.c.write(f, indent + 1)
            print("  " * indent + "}", file=f)
        elif self.loop_type == 1:
            var = gen_var_name()
            print("  " * indent + "int %s = 0;" % var, file=f)
            print("  " * indent + "while (%s < %d) {" % (var, self.n), file=f)
            self.c.write(f, indent + 1)
            print("  " * (indent + 1) + "%s += 1;" % var, file=f)
            print("  " * indent + "}", file=f)
        else:
            var = gen_var_name()
            print("  " * indent + "int %s = 0;" % var, file=f)
            print("  " * indent + "do {", file=f)
            self.c.write(f, indent + 1)
            print("  " * (indent + 1) + "%s += 1;" % var, file=f)
            print("  " * indent + "} until (%s >= %d);" % (var, self.n), file=f)

def write_function(f, name, body, inline=False, inline_ref=False, method_id=None):
    print("_ %s(int x)" % name, file=f, end="")
    if inline:
        print(" inline", file=f, end="")
    if inline_ref:
        print(" inline_ref", file=f, end="")
    if method_id is not None:
        print(" method_id(%d)" % method_id, file=f, end="")
    print(" {", file=f)
    for i in range(VAR_CNT):
        print("  int v%d = 0;" % i, file=f)
    body.write(f, 1);
    print("}", file=f)

def gen_code(xl, xr, with_return, loop_depth=0):
    code = []
    for _ in range(random.randint(0, 2)):
        if random.randint(0, 3) == 0 and loop_depth < 3:
            c = gen_code(xl, xr, False, loop_depth + 1)
            code.append(CodeRepeat(random.randint(0, 3), c, random.randint(0, 2)))
        elif xr - xl > 1:
            xmid = random.randrange(xl + 1, xr)
            ret = random.choice((0, 0, 0, 0, 0, 1, 2))
            c1 = gen_code(xl, xmid, ret == 1, loop_depth)
            if random.randrange(5) == 0:
                c2 = CodeEmpty()
            else:
                c2 = gen_code(xmid, xr, ret == 2, loop_depth)
            code.append(CodeIfRange(xl, xmid, c1, c2))
    if with_return:
        if xr - xl == 1:
            code.append(CodeReturn(random.randrange(10**9)))
        else:
            xmid = random.randrange(xl + 1, xr)
            c1 = gen_code(xl, xmid, True, loop_depth)
            c2 = gen_code(xmid, xr, True, loop_depth)
            code.append(CodeIfRange(xl, xmid, c1, c2))
    for _ in range(random.randint(0, 3)):
        pos = random.randint(0, len(code))
        code.insert(pos, CodeAdd(random.randrange(VAR_CNT), random.randint(0, 10**6)))
    if len(code) == 0:
        return CodeEmpty()
    return CodeBlock(code)

class ExecutionError(Exception):
    pass

def compile_func(fc, fif):
    res = subprocess.run([FUNC_EXECUTABLE, "-o", fif, "-SPA", fc], capture_output=True)
    if res.returncode != 0:
        raise ExecutionError(str(res.stderr, "utf-8"))

def runvm(compiled_fif, xl, xr):
    runner = os.path.join(TMP_DIR, "runner.fif")
    with open(runner, "w") as f:
        print("\"%s\" include <s constant code" % compiled_fif, file=f)
        for x in range(xl, xr):
            print("%d 0 code 1 runvmx abort\"exitcode is not 0\" .s cr { drop } depth 1- times" % x, file=f)
    res = subprocess.run([FIFT_EXECUTABLE, "-I", FIFT_LIBS, runner], capture_output=True)
    if res.returncode != 0:
        raise ExecutionError(str(res.stderr, "utf-8"))
    output = []
    for s in str(res.stdout, "utf-8").split("\n"):
        if s.strip() != "":
            output.append(list(map(int, s.split())))
    return output

cnt_ok = 0
cnt_fail = 0
for test_id in range(0, 1000000):
    random.seed(test_id)
    inline = random.randint(0, 2)
    xr = random.randint(1, 15)
    var_idx = 0
    code = gen_code(0, xr, True)
    fc = os.path.join(TMP_DIR, "code.fc")
    fif = os.path.join(TMP_DIR, "compiled.fif")
    with open(fc, "w") as f:
        print("int in(int x, int l, int r) impure { return (l <= x) & (x < r); }", file=f)
        write_function(f, "foo", code, inline=(inline == 1), inline_ref=(inline == 2))
        print("_ main(int x) {", file=f)
        print("  (int ret, %s) = foo(x);" % ", ".join("int v%d" % i for i in range(VAR_CNT)), file=f)
        print("  return (ret, %s, %d);" % (", ".join("v%d" % i for i in range(VAR_CNT)), MAGIC), file=f)
        print("}", file=f)
    compile_func(fc, fif)
    ok = True
    try:
        output = runvm(fif, 0, xr)
        for x in range(xr):
            my_out = code.execute(State(x)) + [MAGIC]
            fc_out = output[x]
            if my_out != fc_out:
                ok = False
                break
    except ExecutionError:
        ok = False
    if ok:
        cnt_ok += 1
    else:
        cnt_fail += 1
    print("Test %-6d %-6s ok:%-6d fail:%-6d" % (test_id, "OK" if ok else "FAIL", cnt_ok, cnt_fail), file=sys.stderr)
