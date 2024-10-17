#!/usr/bin/env python3

import os
import random
import subprocess
import sys
import tempfile
from typing import List, Optional, Union

def getenv(name: str, default: Optional[str] = None) -> str:
    if name in os.environ:
        return os.environ[name]
    if default is not None:
        return default
    print(f"Environment variable {name} is not set", file=sys.stderr)
    sys.exit(1)

VAR_CNT = 10
TMP_DIR = tempfile.mkdtemp()
FUNC_EXECUTABLE = getenv("FUNC_EXECUTABLE", "func")
FIFT_EXECUTABLE = getenv("FIFT_EXECUTABLE", "fift")
FIFT_LIBS = getenv("FIFT_LIBS")
MAGIC = 123456789

var_idx = 0
def gen_var_name() -> str:
    global var_idx
    var_idx += 1
    return f"i{var_idx}"

class State:
    def __init__(self, x: int):
        self.x = x
        self.vs = [0] * VAR_CNT

    def copy(self):
        s = State(self.x)
        s.vs = self.vs.copy()
        return s

    def copy_from(self, s: 'State'):
        self.x = s.x
        self.vs = s.vs.copy()

class Code:
    def execute(self, state: State) -> Optional[List[int]]:
        raise NotImplementedError

    def write(self, f, indent: int = 0):
        raise NotImplementedError

class CodeEmpty(Code):
    def execute(self, state: State) -> None:
        return None

    def write(self, f, indent: int = 0):
        pass

class CodeReturn(Code):
    def __init__(self, value: int):
        self.value = value

    def execute(self, state: State) -> List[int]:
        return [self.value] + state.vs

    def write(self, f, indent: int = 0):
        print(f"{'  ' * indent}return ({self.value}, {', '.join(f'v{i}' for i in range(VAR_CNT))});", file=f)

class CodeAdd(Code):
    def __init__(self, i: int, value: int):
        self.i = i
        self.value = value

    def execute(self, state: State) -> None:
        state.vs[self.i] += self.value
        return None

    def write(self, f, indent: int = 0):
        print(f"{'  ' * indent}v{self.i} += {self.value};", file=f)

class CodeBlock(Code):
    def __init__(self, code: List[Code]):
        self.code = code

    def execute(self, state: State) -> Optional[List[int]]:
        for c in self.code:
            res = c.execute(state)
            if res is not None:
                return res
        return None

    def write(self, f, indent: int = 0):
        for c in self.code:
            c.write(f, indent)

class CodeIfRange(Code):
    def __init__(self, l: int, r: int, c1: Code, c2: Code):
        self.l = l
        self.r = r
        self.c1 = c1
        self.c2 = c2

    def execute(self, state: State) -> Optional[List[int]]:
        if self.l <= state.x < self.r:
            return self.c1.execute(state)
        else:
            return self.c2.execute(state)

    def write(self, f, indent: int = 0):
        print(f"{'  ' * indent}if (in(x, {self.l}, {self.r})) {{", file=f)
        self.c1.write(f, indent + 1)
        if isinstance(self.c2, CodeEmpty):
            print(f"{'  ' * indent}}}", file=f)
        else:
            print(f"{'  ' * indent}}} else {{", file=f)
            self.c2.write(f, indent + 1)
            print(f"{'  ' * indent}}}", file=f)

class CodeRepeat(Code):
    def __init__(self, n: int, c: Code, loop_type: int):
        self.n = max(n, 1) if loop_type == 2 else n
        self.c = c
        self.loop_type = loop_type

    def execute(self, state: State) -> Optional[List[int]]:
        for _ in range(self.n):
            res = self.c.execute(state)
            if res is not None:
                return res
        return None

    def write(self, f, indent: int = 0):
        if self.loop_type == 0:
            print(f"{'  ' * indent}repeat ({self.n}) {{", file=f)
            self.c.write(f, indent + 1)
            print(f"{'  ' * indent}}}", file=f)
        elif self.loop_type == 1:
            var = gen_var_name()
            print(f"{'  ' * indent}int {var} = 0;", file=f)
            print(f"{'  ' * indent}while ({var} < {self.n}) {{", file=f)
            self.c.write(f, indent + 1)
            print(f"{'  ' * (indent + 1)}{var} += 1;", file=f)
            print(f"{'  ' * indent}}}", file=f)
        elif self.loop_type == 2:
            var = gen_var_name()
            print(f"{'  ' * indent}int {var} = 0;", file=f)
            print(f"{'  ' * indent}do {{", file=f)
            self.c.write(f, indent + 1)
            print(f"{'  ' * (indent + 1)}{var} += 1;", file=f)
            print(f"{'  ' * indent}}} until ({var} >= {self.n});", file=f)
        else:
            var = gen_var_name()
            print(f"{'  ' * indent}int {var} = {self.n - 1};", file=f)
            print(f"{'  ' * indent}while ({var} >= 0) {{", file=f)
            self.c.write(f, indent + 1)
            print(f"{'  ' * (indent + 1)}{var} -= 1;", file=f)
            print(f"{'  ' * indent}}}", file=f)

class CodeThrow(Code):
    def execute(self, state: State) -> str:
        return "EXCEPTION"

    def write(self, f, indent: int = 0):
        print(f"{'  ' * indent}throw(42);", file=f)

class CodeTryCatch(Code):
    def __init__(self, c1: Code, c2: Code):
        self.c1 = c1
        self.c2 = c2

    def execute(self, state: State) -> Optional[List[int]]:
        state0 = state.copy()
        res = self.c1.execute(state)
        if res == "EXCEPTION":
            state.copy_from(state0)
            return self.c2.execute(state)
        else:
            return res

    def write(self, f, indent: int = 0):
        print(f"{'  ' * indent}try {{", file=f)
        self.c1.write(f, indent + 1)
        print(f"{'  ' * indent}}} catch (_, _) {{", file=f)
        self.c2.write(f, indent + 1)
        print(f"{'  ' * indent}}}", file=f)

def write_function(f, name: str, body: Code, inline: bool = False, inline_ref: bool = False, method_id: Optional[int] = None):
    print(f"_ {name}(int x)", file=f, end="")
    if inline:
        print(" inline", file=f, end="")
    if inline_ref:
        print(" inline_ref", file=f, end="")
    if method_id is not None:
        print(f" method_id({method_id})", file=f, end="")
    print(" {", file=f)
    for i in range(VAR_CNT):
        print(f"  int v{i} = 0;", file=f)
    body.write(f, 1)
    print("}", file=f)

def gen_code(xl: int, xr: int, with_return: bool, loop_depth: int = 0, try_catch_depth: int = 0, can_throw: bool = False) -> Code:
    if try_catch_depth < 3 and random.randint(0, 5) == 0:
        c1 = gen_code(xl, xr, with_return, loop_depth, try_catch_depth + 1, random.randint(0, 1) == 0)
        c2 = gen_code(xl, xr, with_return, loop_depth, try_catch_depth + 1, can_throw)
        return CodeTryCatch(c1, c2)
    code = []
    for _ in range(random.randint(0, 2)):
        if random.randint(0, 3) == 0 and loop_depth < 3:
            c = gen_code(xl, xr, False, loop_depth + 1, try_catch_depth, can_throw)
            code.append(CodeRepeat(random.randint(0, 3), c, random.randint(0, 3)))
        elif xr - xl > 1:
            xmid = random.randrange(xl + 1, xr)
            ret = random.choice((0, 0, 0, 0, 0, 1, 2))
            c1 = gen_code(xl, xmid, ret == 1, loop_depth, try_catch_depth, can_throw)
            if random.randrange(5) == 0:
                c2 = CodeEmpty()
            else:
                c2 = gen_code(xmid, xr, ret == 2, loop_depth, try_catch_depth, can_throw)
            code.append(CodeIfRange(xl, xmid, c1, c2))
    if xr - xl == 1 and can_throw and random.randint(0, 5) == 0:
        code.append(CodeThrow())
    if with_return:
        if xr - xl == 1:
            code.append(CodeReturn(random.randrange(10**9)))
        else:
            xmid = random.randrange(xl + 1, xr)
            c1 = gen_code(xl, xmid, True, loop_depth, try_catch_depth, can_throw)
            c2 = gen_code(xmid, xr, True, loop_depth, try_catch_depth, can_throw)
            code.append(CodeIfRange(xl, xmid, c1, c2))
    for _ in range(random.randint(0, 3)):
        pos = random.randint(0, len(code))
        code.insert(pos, CodeAdd(random.randrange(VAR_CNT), random.randint(0, 10**6)))
    if len(code) == 0:
        return CodeEmpty()
    return CodeBlock(code)

class ExecutionError(Exception):
    pass

def compile_func(fc: str, fif: str):
    res = subprocess.run([FUNC_EXECUTABLE, "-o", fif, "-SPA", fc], capture_output=True)
    if res.returncode != 0:
        raise ExecutionError(res.stderr.decode("utf-8"))

def runvm(compiled_fif: str, xl: int, xr: int) -> List[List[int]]:
    runner = os.path.join(TMP_DIR, "runner.fif")
    with open(runner, "w") as f:
        print(f'"{compiled_fif}" include <s constant code', file=f)
        for x in range(xl, xr):
            print(f'{x} 0 code 1 runvmx abort"exitcode is not 0" .s cr {{ drop }} depth 1- times', file=f)
    res = subprocess.run([FIFT_EXECUTABLE, "-I", FIFT_LIBS, runner], capture_output=True)
    if res.returncode != 0:
        raise ExecutionError(res.stderr.decode("utf-8"))
    output = []
    for s in res.stdout.decode("utf-8").split("\n"):
        if s.strip() != "":
            output.append(list(map(int, s.split())))
    return output

def main():
    cnt_ok = 0
    cnt_fail = 0
    for test_id in range(1000000):
        random.seed(test_id)
        inline = random.randint(0, 2)
        xr = random.randint(1, 15)
        global var_idx
        var_idx = 0
        code = gen_code(0, xr, True)
        fc = os.path.join(TMP_DIR, "code.fc")
        fif = os.path.join(TMP_DIR, "compiled.fif")
        with open(fc, "w") as f:
            print("int in(int x, int l, int r) impure { return (l <= x) & (x < r); }", file=f)
            write_function(f, "foo", code, inline=(inline == 1), inline_ref=(inline == 2))
            print("_ main(int x) {", file=f)
            print(f"  (int ret, {', '.join(f'int v{i}' for i in range(VAR_CNT))}) = foo(x);", file=f)
            print(f"  return (ret, {', '.join(f'v{i}' for i in range(VAR_CNT))}, {MAGIC});", file=f)
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
        print(f"Test {test_id:<6} {'OK' if ok else 'FAIL':<6} ok:{cnt_ok:<6} fail:{cnt_fail:<6}", file=sys.stderr)

if __name__ == "__main__":
    main()