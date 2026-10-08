# selfhost/codegen.cpy - AOT compiler backend: cpy AST -> C source.
#
# Scope (v1): numeric-only cpy. Functions whose parameters, locals, return
# values and literals are all int/float/bool/None. No strings, lists,
# dicts, classes, or closures. This restriction is not arbitrary: it means
# no generated code ever touches a heap-allocated Value, so none of the
# runtime's reference counting has to be reproduced by the generator --
# which is exactly what keeps this compiler both simple and safe.
#
# The generated C calls straight into the SAME runtime the tree-walking
# interpreter uses (binop(), val_cmpop(), call_value(), import_module(),
# get_attr(), ...) so a compiled int/float/bool "+" is exactly as correct
# as the interpreter's, and `import` / calls into a module's functions work
# by generating the same generic runtime calls the interpreter itself would
# make -- but a call between two of YOUR OWN compiled functions becomes a
# real, direct C call with no AST node to visit and no Env slot indirection.

BIN_OP_C = {
    "+": "OP_ADD", "-": "OP_SUB", "*": "OP_MUL", "/": "OP_DIV",
    "//": "OP_FDIV", "%": "OP_MOD", "**": "OP_POW",
}
CMP_C = {"<": "C_LT", "<=": "C_LE", ">": "C_GT", ">=": "C_GE"}

# Builtins it's safe to call in expression position without knowing their
# arity ahead of time -- resolved once from g_builtins at program start.
# (print is included here too now, usable as an expression or a statement.)
KNOWN_BUILTINS = {"print", "abs", "round", "min", "max", "pow", "divmod", "int", "float"}


# ============================================================================
# Unboxed ("fast") tier -- Priority 1 of the cpy upgrade plan.
#
# Every function above still gets its ordinary boxed C body (Value in,
# Value out, going through binop()/val_cmpop() like today) -- that part is
# completely unchanged and is always correct by construction, so it stays
# the fallback for anything the analysis below can't prove safe to unbox.
#
# ADDITIONALLY, for a function whose parameters, locals and return value
# can be proven -- via the whole-program type inference in TypeInfer below
# -- to always be exactly one concrete numeric C type (int64_t or double,
# never a mix, never anything else), Gen also emits a second variant,
# `<name>__fast`, that works entirely in that raw C type: no Value, no
# tag checks, no calls through binop(). A *call site* only gets rewritten
# to use `<name>__fast` when the argument expressions are ALSO proven (by
# the same analysis) to have a type that matches (or safely widens into)
# the callee's fast parameter types. Recursive/self calls are exactly the
# common case this catches (see fib below), so the entire hot recursion
# tree of something like fib() ends up running fully unboxed, with only a
# single box/unbox at the outer boundary where a boxed caller invokes it.
#
# Soundness: TypeInfer only ever *grows* a variable's type from "unknown"
# (T_PENDING) towards a concrete type by unifying every site that could
# produce a value for it (every assignment, every call argument, every
# return) with STRICT equality (see strict_unify) -- int and float are
# NEVER silently merged/promoted across different sites of the same
# variable, because that would change an observable result (e.g. a
# function that returns int on one path and float on another must keep
# doing exactly that; specializing it to "always returns double" would be
# a real semantic change, not just an optimization). The one place actual
# int->float promotion happens is arith_unify, which computes the type of
# a SINGLE arithmetic expression (e.g. `int_expr + float_expr`) -- that's
# not a cross-site merge, it's just describing Python's own `+` semantics
# for that one expression, exactly like binop() already does at runtime.
# A function only gets a `__fast` variant when every part of it resolves
# to a concrete type this way; anything else (a mixed-type variable, `**`,
# a call to a builtin/module function, strings, a return path that can
# fall through without a value, ...) simply leaves that function on the
# boxed-only path -- always correct, purely an optimization opportunity
# left on the table, never a risk of wrong output.
# ============================================================================

T_PENDING = "?"   # not yet constrained by any evidence (grows monotonically)
T_POISON = "!"    # proven NOT representable as a single concrete numeric type


def strict_unify(a, b):
    """Cross-site merge: what a variable/return/param's type must be, given
    it can come from (at least) two different places. No promotion -- if
    the two sites disagree on a concrete type, that's a real potential
    difference in observable behavior, so we give up on unboxing (POISON)
    rather than guess wrong."""
    if a == T_POISON or b == T_POISON:
        return T_POISON
    if a == T_PENDING:
        return b
    if b == T_PENDING:
        return a
    if a == b:
        return a
    return T_POISON


def arith_unify(a, b):
    """Type of a single arithmetic expression combining two operand types
    (for +, -, *, //, %). This is where int/float promotion legitimately
    happens -- it's describing what binop() itself does for one specific
    computed value, not merging across unrelated sites. bool behaves as
    int here (True + 1 == 2, an int, not a bool) -- matches is_intlike()
    in interp.c's binop()."""
    if a == T_POISON or b == T_POISON:
        return T_POISON
    if a == T_PENDING or b == T_PENDING:
        return T_PENDING
    a2 = "i" if a == "b" else a
    b2 = "i" if b == "b" else b
    if a2 == b2:
        return a2
    return "f"


def ctype_of(t):
    return "double" if t == "f" else "int64_t"


def is_nonneg_int_const(n):
    """A plain non-negative integer literal (not bool, not an expression)
    -- the one case where `**`'s result type (int vs float, which binop()
    picks based on the exponent's RUNTIME SIGN) is provable at compile
    time, so the fast tier can support it. See the "**" handling in
    TypeInfer.expr_type and Gen.gen_expr_fast's "binop" case."""
    if n["kind"] != "const":
        return False
    v = n["value"]
    if v is True or v is False:
        return False
    return isinstance(v, int) and v >= 0


def collect_names(x, out):
    """Every N_NAME reference reachable inside x (a node, a list of nodes,
    or a list of tuples like kwargs/with-items), added to `out`. Fully
    generic over the dict-based AST's many node shapes (binop/call/with/
    try/fstring/... all have different field names) rather than
    enumerating each one -- deliberately so adding a new node kind to the
    parser later can't silently make this under-count (the failure mode
    of a kind-by-kind walker, which is exactly how two earlier bugs this
    session happened). Used by compile_module to find which top-level
    def/class statements are actually reachable from code that runs, so
    an unused one (and whatever only IT depends on) doesn't have to be
    AOT-compilable to compile the rest of the file."""
    if isinstance(x, dict):
        if x.get("kind") == "name":
            out.add(x["id"])
        for k in x:
            if k == "kind" or k == "line" or k == "op" or k == "id":
                continue
            collect_names(x[k], out)
    elif isinstance(x, list):
        for item in x:
            collect_names(item, out)
    elif isinstance(x, tuple):
        for item in x:
            collect_names(item, out)


def call_arg_ok(arg_t, param_t):
    """Can an argument of static type arg_t be passed directly into a fast
    parameter declared param_t? Only safe, lossless widenings are allowed
    (int/bool -> float); never float -> int (would truncate/change the
    value) and never anything -> bool (bool is only ever produced by
    comparisons/`not`/literals, never implicitly)."""
    if arg_t not in ("i", "f", "b") or param_t not in ("i", "f", "b"):
        return False
    if param_t == "f":
        return True
    if param_t == "i":
        return arg_t == "i" or arg_t == "b"
    return arg_t == "b"


def cast_c(code, from_t, to_t):
    if from_t == to_t:
        return code
    if to_t == "f":
        return f"(double)({code})"
    return code  # i<->b share the same C representation (int64_t 0/1/N)


def unbox_field(t):
    return ".d" if t == "f" else ".i"  # i and b are both stored in the union's .i


def box_result(code, t):
    if t == "f":
        return f"V_float({code})"
    if t == "b":
        return f"V_bool((int)({code}))"
    return f"V_int({code})"


class TypeInfer:
    """Whole-program numeric type inference for the fast tier. Runs BEFORE
    Gen does any codegen, over the same raw AST Gen will process -- it
    never raises: anything it doesn't understand just fails to resolve to
    a concrete type (poisoning that one function's fast eligibility), and
    the ordinary boxed Gen pass remains the authority on whether the
    program is valid cpy AOT input at all."""

    def __init__(self, prog):
        self.defs = {}
        self.param_t = {}
        self.ret_t = {}
        self.locals_t = {}
        self.top_t = {}
        self.poisoned = {}
        self.always_ret = {}
        for s in prog:
            if s["kind"] == "def":
                self.defs[s["name"]] = s
                self.param_t[s["name"]] = [T_PENDING for _p in s["params"]]
                self.ret_t[s["name"]] = T_PENDING
                self.locals_t[s["name"]] = {}
                self.poisoned[s["name"]] = bool(s["defaults"])
                self.always_ret[s["name"]] = self.check_always_returns(s["body"])
        self.top_stmts = [s for s in prog if s["kind"] not in ("def", "import")]
        self.changed = False
        self.run()

    # ---- "does every path through this block end in `return <expr>`?" ----
    # Purely syntactic and deliberately conservative: it only recognizes a
    # trailing `return expr` or an if/else where BOTH arms always return.
    # A while/for loop is never considered guaranteed (it might not
    # execute), which is safe -- it just means fewer functions qualify,
    # never a wrong answer.
    def check_always_returns(self, stmts):
        for s in stmts:
            k = s["kind"]
            if k == "return" and s["value"] is not None:
                return True
            if k == "if" and s["orelse"]:
                if self.check_always_returns(s["body"]) and self.check_always_returns(s["orelse"]):
                    return True
        return False

    # ---------------------------------------------------------- lookups
    def var_type(self, fn, name):
        if fn is None:
            return self.top_t.get(name, T_PENDING)
        params = self.defs[fn]["params"]
        if name in params:
            return self.param_t[fn][params.index(name)]
        return self.locals_t[fn].get(name, T_PENDING)

    def mark_poison(self, fn):
        if fn is not None and not self.poisoned.get(fn, False):
            self.poisoned[fn] = True
            self.changed = True

    def bump_var(self, fn, name, t):
        old = self.var_type(fn, name)
        new = strict_unify(old, t)
        if new == old:
            return
        if fn is None:
            self.top_t[name] = new
        else:
            params = self.defs[fn]["params"]
            if name in params:
                self.param_t[fn][params.index(name)] = new
            else:
                self.locals_t[fn][name] = new
        self.changed = True
        if new == T_POISON:
            self.mark_poison(fn)

    def bump_ret(self, fn, t):
        if fn is None:
            return
        old = self.ret_t[fn]
        new = strict_unify(old, t)
        if new == old:
            return
        self.ret_t[fn] = new
        self.changed = True
        if new == T_POISON:
            self.mark_poison(fn)

    # ------------------------------------------------------- expressions
    # Mirrors Gen.gen_expr's node kinds; returns a type ('i'/'f'/'b',
    # T_PENDING, or T_POISON) and, for calls, ALSO propagates the argument
    # types into the callee's parameters (the cross-function part of the
    # inference -- this is what lets `fib(30)` at the top level seed
    # fib's own parameter type, which then flows into fib's recursive
    # self-calls too).
    def expr_type(self, fn, n):
        k = n["kind"]
        if k == "const":
            v = n["value"]
            if v is None:
                return T_POISON
            if v is True or v is False:
                return "b"
            if isinstance(v, int):
                return "i"
            if isinstance(v, float):
                return "f"
            return T_POISON
        if k == "name":
            nm = n["id"]
            if nm in self.defs:
                return T_POISON  # a bare function name used as a value
            return self.var_type(fn, nm)
        if k == "binop":
            op = n["op"]
            lt = self.expr_type(fn, n["left"])
            rt = self.expr_type(fn, n["right"])
            if op == "**":
                if lt == T_POISON or rt == T_POISON:
                    return T_POISON
                if lt == T_PENDING or rt == T_PENDING:
                    return T_PENDING
                if lt == "f" or rt == "f":
                    return "f"  # float ** anything is always float -- no sign ambiguity
                if is_nonneg_int_const(n["right"]):
                    return "i"  # int ** (provably non-negative int) is always int
                return T_POISON  # int ** int of unprovable sign: result type is runtime-dependent (see binop())
            if op == "/":
                if lt == T_POISON or rt == T_POISON:
                    return T_POISON
                if lt == T_PENDING or rt == T_PENDING:
                    return T_PENDING
                return "f"
            if op == "+" or op == "-" or op == "*" or op == "//" or op == "%":
                return arith_unify(lt, rt)
            return T_POISON
        if k == "cmp":
            lt = self.expr_type(fn, n["left"])
            rt = self.expr_type(fn, n["right"])
            if lt == T_POISON or rt == T_POISON:
                return T_POISON
            if lt == T_PENDING or rt == T_PENDING:
                return T_PENDING
            return "b"
        if k == "and" or k == "or":
            lt = self.expr_type(fn, n["left"])
            rt = self.expr_type(fn, n["right"])
            return strict_unify(lt, rt)
        if k == "not":
            vt = self.expr_type(fn, n["value"])
            if vt == T_POISON:
                return T_POISON
            if vt == T_PENDING:
                return T_PENDING
            return "b"
        if k == "unary":
            vt = self.expr_type(fn, n["value"])
            if vt == T_POISON or vt == T_PENDING:
                return vt
            if n["op"] == "-" or n["op"] == "+":
                return "i" if vt == "b" else vt
            return T_POISON
        if k == "call":
            return self.call_type(fn, n)
        return T_POISON

    def call_type(self, fn, n):
        fnode = n["fn"]
        if n.get("kwargs"):
            # AOT's fast/unboxed tier works entirely off positional
            # parameter slots -- it has no notion of binding a keyword
            # argument to a parameter. Still visit every value for its own
            # side effects (same reasoning as the builtin-call branch just
            # below), just never specialize a call that uses kwargs.
            for a in n["args"]:
                self.expr_type(fn, a)
            for _kw, a in n["kwargs"]:
                self.expr_type(fn, a)
            return T_POISON
        if fnode["kind"] != "name" or fnode["id"] not in self.defs:
            # A call to a builtin, a module function, or anything else we
            # don't specialize. We still have to visit the ARGUMENTS for
            # their own side effects (they might contain calls to OUR
            # functions, e.g. `print(fib(10))` -- that nested fib(10) call
            # is exactly what seeds fib's own parameter type) -- we just
            # can't say anything about the type of the call's own result.
            for a in n["args"]:
                self.expr_type(fn, a)
            return T_POISON
        name = fnode["id"]
        args_t = [self.expr_type(fn, a) for a in n["args"]]
        params = self.defs[name]["params"]
        if len(args_t) != len(params):
            return T_POISON
        i = 0
        while i < len(args_t):
            if args_t[i] != T_PENDING:
                self.bump_var(name, params[i], args_t[i])
            i += 1
        return self.ret_t[name]

    # -------------------------------------------------------- statements
    def visit_stmts(self, fn, stmts):
        for s in stmts:
            self.visit_stmt(fn, s)

    def visit_stmt(self, fn, s):
        k = s["kind"]
        if k == "exprstmt":
            self.expr_type(fn, s["value"])
        elif k == "assign":
            if s["target"]["kind"] != "name":
                self.mark_poison(fn)
                return
            vt = self.expr_type(fn, s["value"])
            self.bump_var(fn, s["target"]["id"], vt)
        elif k == "augassign":
            if s["target"]["kind"] != "name":
                self.mark_poison(fn)
                return
            tname = s["target"]["id"]
            cur_t = self.var_type(fn, tname)
            rt = self.expr_type(fn, s["value"])
            op = s["op"]
            if op == "**":
                if cur_t == "f" or rt == "f":
                    self.bump_var(fn, tname, "f")
                elif is_nonneg_int_const(s["value"]):
                    self.bump_var(fn, tname, "i")
                else:
                    self.bump_var(fn, tname, T_POISON)
            elif op == "/":
                if cur_t == T_POISON or rt == T_POISON:
                    self.bump_var(fn, tname, T_POISON)
                elif cur_t != T_PENDING and rt != T_PENDING:
                    self.bump_var(fn, tname, "f")
            elif op == "+" or op == "-" or op == "*" or op == "//" or op == "%":
                self.bump_var(fn, tname, arith_unify(cur_t, rt))
            else:
                self.bump_var(fn, tname, T_POISON)
        elif k == "if":
            self.expr_type(fn, s["cond"])
            self.visit_stmts(fn, s["body"])
            self.visit_stmts(fn, s["orelse"])
        elif k == "while":
            self.expr_type(fn, s["cond"])
            self.visit_stmts(fn, s["body"])
        elif k == "for":
            self.visit_for(fn, s)
        elif k == "return":
            if s["value"] is None:
                self.bump_ret(fn, T_POISON)
            else:
                self.bump_ret(fn, self.expr_type(fn, s["value"]))
        elif k == "break" or k == "continue" or k == "pass":
            pass
        else:
            self.mark_poison(fn)

    def visit_for(self, fn, s):
        tgt = s["target"]
        it = s["iter"]
        shape_ok = (not isinstance(tgt, list) and it["kind"] == "call"
                    and it["fn"]["kind"] == "name" and it["fn"]["id"] == "range")
        if not shape_ok:
            self.mark_poison(fn)
        else:
            for a in it["args"]:
                at = self.expr_type(fn, a)
                if at != T_PENDING and at != "i" and at != "b":
                    self.mark_poison(fn)
            self.bump_var(fn, tgt, "i")
        self.visit_stmts(fn, s["body"])

    # ---------------------------------------------------------- driver
    def run(self):
        i = 0
        while i < 40:
            self.changed = False
            for name in self.defs:
                self.visit_stmts(name, self.defs[name]["body"])
            self.visit_stmts(None, self.top_stmts)
            if not self.changed:
                break
            i += 1
        self.finalize()

    def finalize(self):
        def struct_ok(name):
            if self.poisoned[name]:
                return False
            if not self.always_ret.get(name, False):
                return False
            for t in self.param_t[name]:
                if t != "i" and t != "f" and t != "b":
                    return False
            rt = self.ret_t[name]
            return rt == "i" or rt == "f" or rt == "b"

        self.eligible = {}
        for name in self.defs:
            self.eligible[name] = struct_ok(name)

        # Shrinking fixpoint: a function also needs every user-function
        # call inside it to resolve to a *currently* eligible callee with
        # argument types that fit its fast parameters -- eligibility can
        # only be revoked here, never (re-)granted, so this always
        # terminates and the order of iteration doesn't affect the result.
        changed = True
        guard = 0
        while changed and guard <= len(self.defs):
            changed = False
            guard += 1
            for name in self.defs:
                if self.eligible[name] and not self.calls_all_ok(name):
                    self.eligible[name] = False
                    changed = True

    def arg_representable(self, fn, node):
        """Can this expression, appearing as an argument somewhere inside a
        fast function body, be produced as a Value to hand to a boxed/
        builtin call? Either it's directly fast-typed (box it), or it's
        itself a boxable call whose own arguments recursively satisfy this
        -- e.g. print(abs(x)) or print(helper(x, y)) to arbitrary depth."""
        if self.expr_type(fn, node) in ("i", "f", "b"):
            return True
        if node["kind"] == "call":
            return self.stmt_call_boxable(fn, node)
        return False

    def stmt_call_boxable(self, fn, call_node):
        """Can this call, appearing as a bare statement (its result
        discarded) inside fast function `fn`, be compiled as "box each
        argument, call the boxed/builtin entry point, discard the
        result"? This is what lets a numeric function call print()/a
        non-fast sibling/etc. as a side-effecting statement and still get
        the fast tier for the rest of its body -- see gen_boxed_call_stmt_fast
        in Gen. Only statement position qualifies: if the result were used
        further (assigned, returned, part of an expression), we'd need to
        unbox it back into a concrete i/f/b to keep computing in the fast
        domain, which isn't attempted here (that call site stays
        disqualifying -- see walk_expr's "call" case above)."""
        if call_node.get("kwargs"):
            return False  # see call_type's matching kwargs guard -- no positional-slot binding for kwargs here either
        fnode = call_node["fn"]
        if fnode["kind"] != "name":
            return False
        callee = fnode["id"]
        if callee not in self.defs and callee not in KNOWN_BUILTINS:
            return False
        for a in call_node["args"]:
            if not self.arg_representable(fn, a):
                return False
        return True

    def calls_all_ok(self, name):
        result = [True]

        def walk_expr(n):
            k = n["kind"]
            if k == "call":
                fnode = n["fn"]
                if fnode["kind"] == "name" and fnode["id"] in self.defs and not n.get("kwargs"):
                    callee = fnode["id"]
                    if not self.eligible.get(callee, False):
                        result[0] = False
                    else:
                        cparams = self.param_t[callee]
                        if len(n["args"]) != len(cparams):
                            result[0] = False
                        else:
                            j = 0
                            while j < len(cparams):
                                at = self.expr_type(name, n["args"][j])
                                if not call_arg_ok(at, cparams[j]):
                                    result[0] = False
                                j += 1
                else:
                    # Not a direct fast-to-fast call (builtin, module call, or
                    # a user function whose own eligibility/arg types don't
                    # line up). walk_stmt's "exprstmt" case below separately
                    # allows exactly this when the call's result is discarded
                    # (boxes the arguments, calls the boxed/builtin entry
                    # point) -- but that only covers a call in STATEMENT
                    # position. Here the call's result feeds into further
                    # fast-domain computation (e.g. `x = abs(n) + 1`), and
                    # there's no fast-typed value to hand back, so this is
                    # disqualifying.
                    result[0] = False
                for a in n["args"]:
                    walk_expr(a)
                for _kw, a in n.get("kwargs") or []:
                    walk_expr(a)
                return
            if k == "binop" or k == "cmp" or k == "and" or k == "or":
                walk_expr(n["left"])
                walk_expr(n["right"])
                return
            if k == "not" or k == "unary":
                walk_expr(n["value"])
                return

        def walk_stmt(s):
            k = s["kind"]
            if k == "exprstmt":
                v = s["value"]
                if not (v["kind"] == "call" and self.stmt_call_boxable(name, v)):
                    # stmt_call_boxable (when it returns True) already
                    # recursively validated the ENTIRE argument subtree via
                    # arg_representable -- nested boxable calls included --
                    # so there is nothing left for walk_expr's stricter
                    # (fast-to-fast-only) rule to check in that case. Only
                    # fall back to it when the call isn't boxable at all.
                    walk_expr(v)
            elif k == "assign" or k == "augassign":
                walk_expr(s["value"])
            elif k == "return":
                if s["value"] is not None:
                    walk_expr(s["value"])
            elif k == "if":
                walk_expr(s["cond"])
                for x in s["body"]:
                    walk_stmt(x)
                for x in s["orelse"]:
                    walk_stmt(x)
            elif k == "while":
                walk_expr(s["cond"])
                for x in s["body"]:
                    walk_stmt(x)
            elif k == "for":
                for a in s["iter"]["args"]:
                    walk_expr(a)
                for x in s["body"]:
                    walk_stmt(x)

        for s in self.defs[name]["body"]:
            walk_stmt(s)
        return result[0]


class CompileError(Exception):
    pass


def c_name(name):
    return "u_" + name


def c_str_lit(s):
    """A C string literal for s. Used for embedding the source filename
    into generated Frame.file fields (traceback "File ..." lines) -- not
    a general string-literal codegen path (AOT v1 doesn't compile Python
    string constants at all yet; see gen_expr's "const" case)."""
    out = ['"']
    for ch in s:
        if ch == "\\" or ch == '"':
            out.append("\\" + ch)
        elif ch == "\n":
            out.append("\\n")
        elif ord(ch) < 0x20:
            out.append("\\x%02x" % ord(ch))
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def mod_c_name(name):
    return "mod_" + name.replace(".", "_")


def ext_c_name(name):
    return "bf_" + name


class Gen:
    def __init__(self):
        self.out = []
        self.tmp_counter = 0
        self.functions = {}      # cpy name -> arity, for direct-call resolution
        self.modules = {}        # local bound name -> dotted module name, for `import x as y`
        self.used_builtins = set()
        self.indent = 1
        self.cur_line = 0        # best-effort source line for error messages
        self.filename = "<aot>"  # overwritten by compile_module(); used for traceback "File" lines
        self.cur_fn = None       # name of the boxed function currently being
                                  # generated (None at top level) -- used to
                                  # look up TypeInfer's resolved var types
                                  # when deciding whether a call site can be
                                  # redirected to a callee's fast variant.
        self.tinfer = None       # set by compile_module before any codegen

    def emit(self, line):
        self.out.append(("    " * self.indent) + line)

    def new_tmp(self):
        self.tmp_counter += 1
        return f"_t{self.tmp_counter}"

    def err(self, msg):
        raise CompileError(f"line {self.cur_line}: {msg}")

    # -------------------------------------------------------- expressions
    def gen_expr(self, n):
        k = n["kind"]
        if k == "const":
            v = n["value"]
            if v is None:
                return "V_none()"
            if v is True:
                return "V_bool(1)"
            if v is False:
                return "V_bool(0)"
            if isinstance(v, int):
                return f"V_int({v}LL)"
            if isinstance(v, float):
                return f"V_float({v!r})"
            self.err(f"AOT v1 does not support string/other literals: {v!r}")
        if k == "name":
            if n["id"] in self.modules:
                self.err(f"'{n['id']}' is a module; use {n['id']}.something(...), it isn't a value on its own")
            return c_name(n["id"])
        if k == "binop":
            op = BIN_OP_C.get(n["op"])
            if op is None:
                self.err(f"AOT v1 does not support operator {n['op']!r}")
            return f"binop({op}, {self.gen_expr(n['left'])}, {self.gen_expr(n['right'])})"
        if k == "cmp":
            if n["op"] == "==":
                return f"V_bool(val_eq({self.gen_expr(n['left'])}, {self.gen_expr(n['right'])}))"
            if n["op"] == "!=":
                return f"V_bool(!val_eq({self.gen_expr(n['left'])}, {self.gen_expr(n['right'])}))"
            op = CMP_C.get(n["op"])
            if op is None:
                self.err(f"AOT v1 does not support comparison {n['op']!r} (needs a container)")
            return f"V_bool(val_cmpop({op}, {self.gen_expr(n['left'])}, {self.gen_expr(n['right'])}))"
        if k == "and":
            t = self.new_tmp()
            return f"({{ Value {t} = {self.gen_expr(n['left'])}; val_truthy({t}) ? {self.gen_expr(n['right'])} : {t}; }})"
        if k == "or":
            t = self.new_tmp()
            return f"({{ Value {t} = {self.gen_expr(n['left'])}; val_truthy({t}) ? {t} : {self.gen_expr(n['right'])}; }})"
        if k == "not":
            return f"V_bool(!val_truthy({self.gen_expr(n['value'])}))"
        if k == "unary":
            v = self.gen_expr(n["value"])
            return f"binop(OP_SUB, V_int(0), {v})" if n["op"] == "-" else v
        if k == "call":
            return self.gen_call_expr(n)
        if k == "attr":
            if n["obj"]["kind"] == "name" and n["obj"]["id"] in self.modules:
                mvar = mod_c_name(self.modules[n["obj"]["id"]])
                return f'get_attr({mvar}, INTERN("{n["name"]}"))'
            self.err(
                "AOT v1 only supports attribute access on an imported module "
                "(module.name or module.function(...))")
        self.err(f"AOT v1 does not support {k!r} in expression position (numeric-only subset)")

    def gen_call_expr(self, n):
        fn = n["fn"]
        if n.get("kwargs"):
            # Without this, a builtin/module call below would silently DROP
            # the keyword arguments (args_c only ever collects n["args"]) --
            # wrong generated code, not even an error. A user-function call
            # happens to be caught anyway by the arity check further down,
            # but builtins and module calls have no such check, so guard
            # explicitly and uniformly here instead of relying on that.
            self.err("AOT v1 does not support keyword arguments in calls")

        # Fast-path redirect: a call to one of OUR functions, which the
        # TypeInfer pass proved is fully unboxable AND whose argument
        # expressions here are proven to have types that fit its fast
        # parameters, gets compiled as a direct unboxed call instead of
        # going through binop()/call_value(). This is purely additive: if
        # anything below doesn't line up, we fall straight through to the
        # exact same boxed call this function has always generated.
        if fn["kind"] == "name" and fn["id"] in self.functions and self.tinfer.eligible.get(fn["id"], False):
            callee = fn["id"]
            cparams = self.tinfer.param_t[callee]
            if len(n["args"]) == len(cparams):
                arg_types = [self.tinfer.expr_type(self.cur_fn, a) for a in n["args"]]
                all_ok = True
                for i in range(len(arg_types)):
                    if not call_arg_ok(arg_types[i], cparams[i]):
                        all_ok = False
                if all_ok:
                    fast_args = []
                    i = 0
                    while i < len(n["args"]):
                        boxed_code = self.gen_expr(n["args"][i])
                        raw = f"({boxed_code}){unbox_field(arg_types[i])}"
                        fast_args.append(cast_c(raw, arg_types[i], cparams[i]))
                        i += 1
                    call_c = f"{c_name(callee)}__fast({', '.join(fast_args)})"
                    return box_result(call_c, self.tinfer.ret_t[callee])

        args_c = [self.gen_expr(a) for a in n["args"]]
        argv = ", ".join(args_c)

        if fn["kind"] == "attr" and fn["obj"]["kind"] == "name" and fn["obj"]["id"] in self.modules:
            mod_local = fn["obj"]["id"]
            mvar = mod_c_name(self.modules[mod_local])
            attr = fn["name"]
            return (f'call_value(get_attr({mvar}, INTERN("{attr}")), '
                    f'(Value[]){{{argv}}}, {len(args_c)}, NULL)') if args_c else \
                   f'call_value(get_attr({mvar}, INTERN("{attr}")), NULL, 0, NULL)'

        if fn["kind"] != "name":
            self.err("AOT v1 only supports calling a plain function, or module.function(...)")
        name = fn["id"]
        if name in self.functions:
            arity = self.functions[name]
            if len(args_c) != arity:
                self.err(f"{name}() takes {arity} argument{'s' if arity != 1 else ''}, {len(args_c)} given")
            return f"{c_name(name)}({argv})"
        if name in KNOWN_BUILTINS:
            self.used_builtins.add(name)
            bvar = ext_c_name(name)
            return (f"call_value({bvar}, (Value[]){{{argv}}}, {len(args_c)}, NULL)") if args_c else \
                   f"call_value({bvar}, NULL, 0, NULL)"
        self.err(
            f"AOT v1: call to '{name}' -- this must be either another function defined in "
            f"this file, one of {sorted(KNOWN_BUILTINS)}, or a module.function(...) call "
            f"after `import module`")

    # --------------------------------------------------------- statements
    def gen_stmt(self, n):
        self.cur_line = n.get("line", self.cur_line)
        if self.cur_line:
            self.emit(f"g_line = {self.cur_line}; fr.line = {self.cur_line};")
        k = n["kind"]
        if k == "exprstmt":
            self.emit(f"(void)({self.gen_expr(n['value'])});")
            return
        if k == "assign":
            if n["target"]["kind"] != "name":
                self.err("AOT v1 only supports assigning to a plain name")
            if n["target"]["id"] in self.modules:
                self.err(f"cannot assign to '{n['target']['id']}', it's an imported module name")
            self.emit(f"{c_name(n['target']['id'])} = {self.gen_expr(n['value'])};")
            return
        if k == "augassign":
            if n["target"]["kind"] != "name":
                self.err("AOT v1 only supports augmented assignment to a plain name")
            op = BIN_OP_C.get(n["op"])
            if op is None:
                self.err(f"AOT v1 does not support operator {n['op']!r}=")
            tgt = c_name(n["target"]["id"])
            self.emit(f"{tgt} = binop({op}, {tgt}, {self.gen_expr(n['value'])});")
            return
        if k == "if":
            self.emit(f"if (val_truthy({self.gen_expr(n['cond'])})) {{")
            self.indent += 1
            self.gen_body(n["body"])
            self.indent -= 1
            if n["orelse"]:
                self.emit("} else {")
                self.indent += 1
                self.gen_body(n["orelse"])
                self.indent -= 1
            self.emit("}")
            return
        if k == "while":
            self.emit(f"while (val_truthy({self.gen_expr(n['cond'])})) {{")
            self.indent += 1
            self.gen_body(n["body"])
            self.indent -= 1
            self.emit("}")
            return
        if k == "for":
            self.gen_for(n)
            return
        if k == "return":
            self.emit(f"return {self.gen_expr(n['value']) if n['value'] is not None else 'V_none()'};")
            return
        if k == "break":
            self.emit("break;")
            return
        if k == "continue":
            self.emit("continue;")
            return
        if k == "pass":
            return
        if k == "import":
            self.err("`import` must appear at the top level of the file, not inside a function or block")
        self.err(f"AOT v1 does not support statement kind {k!r}")

    def gen_for(self, n):
        target = n["target"]
        if isinstance(target, list):
            self.err("AOT v1 does not support tuple targets in for-loops")
        it = n["iter"]
        if it["kind"] != "call" or it["fn"]["kind"] != "name" or it["fn"]["id"] != "range":
            self.err("AOT v1 only supports 'for x in range(...)' loops")
        rargs = it["args"]
        if len(rargs) == 1:
            lo, hi, step = "V_int(0LL)", self.gen_expr(rargs[0]), "V_int(1LL)"
        elif len(rargs) == 2:
            lo, hi, step = self.gen_expr(rargs[0]), self.gen_expr(rargs[1]), "V_int(1LL)"
        elif len(rargs) == 3:
            lo, hi, step = self.gen_expr(rargs[0]), self.gen_expr(rargs[1]), self.gen_expr(rargs[2])
        else:
            self.err("range() takes 1 to 3 arguments")
        li, hiv, st = self.new_tmp(), self.new_tmp(), self.new_tmp()
        self.emit("{")
        self.indent += 1
        self.emit(f"int64_t {li} = ({lo}).i, {hiv} = ({hi}).i, {st} = ({step}).i, _i;")
        self.emit(f"for (_i = {li}; {st} > 0 ? _i < {hiv} : _i > {hiv}; _i += {st}) {{")
        self.indent += 1
        self.emit(f"{c_name(target)} = V_int(_i);")
        self.gen_body(n["body"])
        self.indent -= 1
        self.emit("}")
        self.indent -= 1
        self.emit("}")

    def gen_body(self, stmts):
        for s in stmts:
            self.gen_stmt(s)

    # ===================================================== fast (unboxed)
    # Everything below generates the `<name>__fast` variant for a function
    # TypeInfer proved fully numeric-monomorphic. It mirrors the boxed
    # gen_expr/gen_stmt/gen_for above node-for-node, but works in raw
    # int64_t/double throughout instead of Value, and is only ever invoked
    # for functions (and call sites) TypeInfer has already verified are
    # safe -- see the big comment above the TypeInfer class for why that's
    # sound. gen_expr_fast returns a (c_code, type) pair so callers can
    # cast/promote correctly (e.g. mixing an int64_t sub-expression into a
    # double one).

    def gen_expr_fast(self, n, fn):
        k = n["kind"]
        if k == "const":
            v = n["value"]
            if v is True:
                return ("1", "b")
            if v is False:
                return ("0", "b")
            if isinstance(v, int):
                return (f"{v}LL", "i")
            if isinstance(v, float):
                return (f"{v!r}", "f")
            self.err(f"internal: unexpected literal {v!r} reached fast codegen")
        if k == "name":
            return (c_name(n["id"]), self.tinfer.var_type(fn, n["id"]))
        if k == "binop":
            if n["op"] == "**":
                return self.gen_pow_fast(n, fn)
            lc, lt = self.gen_expr_fast(n["left"], fn)
            rc, rt = self.gen_expr_fast(n["right"], fn)
            return self.gen_binop_fast(n["op"], lc, lt, rc, rt)
        if k == "cmp":
            lc, lt = self.gen_expr_fast(n["left"], fn)
            rc, rt = self.gen_expr_fast(n["right"], fn)
            t = arith_unify(lt, rt)
            lc2 = cast_c(lc, lt, t)
            rc2 = cast_c(rc, rt, t)
            cop = {"<": "<", "<=": "<=", ">": ">", ">=": ">=", "==": "==", "!=": "!="}[n["op"]]
            return (f"(({lc2}) {cop} ({rc2}) ? 1 : 0)", "b")
        if k == "and":
            lc, lt = self.gen_expr_fast(n["left"], fn)
            rc, rt = self.gen_expr_fast(n["right"], fn)
            t = self.new_tmp()
            return (f"({{ {ctype_of(lt)} {t} = ({lc}); ({t} != 0) ? ({rc}) : {t}; }})", lt)
        if k == "or":
            lc, lt = self.gen_expr_fast(n["left"], fn)
            rc, rt = self.gen_expr_fast(n["right"], fn)
            t = self.new_tmp()
            return (f"({{ {ctype_of(lt)} {t} = ({lc}); ({t} != 0) ? {t} : ({rc}); }})", lt)
        if k == "not":
            vc, vt = self.gen_expr_fast(n["value"], fn)
            return (f"(({vc}) == 0 ? 1 : 0)", "b")
        if k == "unary":
            vc, vt = self.gen_expr_fast(n["value"], fn)
            if n["op"] == "-":
                if vt == "f":
                    return (f"(-({vc}))", "f")
                return (f"cpy_neg_i({cast_c(vc, vt, 'i')})", "i")
            if vt == "b":
                return (vc, "i")
            return (vc, vt)
        if k == "call":
            return self.gen_call_fast(n, fn)
        self.err(f"internal: unsupported fast expr kind {k!r}")

    def gen_pow_fast(self, n, fn):
        """`**` in the fast tier -- see TypeInfer's "**" handling for why
        only float-involved or non-negative-constant-exponent cases are
        ever reached here (eligibility already proved it's one of those)."""
        lc, lt = self.gen_expr_fast(n["left"], fn)
        rc, rt = self.gen_expr_fast(n["right"], fn)
        if lt == "f" or rt == "f":
            return (f"cpy_pow_f({cast_c(lc, lt, 'f')}, {cast_c(rc, rt, 'f')})", "f")
        return (f"cpy_pow_i({cast_c(lc, lt, 'i')}, {cast_c(rc, rt, 'i')})", "i")

    def gen_binop_fast(self, op, lc, lt, rc, rt):
        if op == "/":
            lc2 = cast_c(lc, lt, "f")
            rc2 = cast_c(rc, rt, "f")
            return (f"cpy_truediv({lc2}, {rc2})", "f")
        rest = arith_unify(lt, rt)
        if rest == "f":
            lc2 = cast_c(lc, lt, "f")
            rc2 = cast_c(rc, rt, "f")
            if op == "+":
                return (f"(({lc2}) + ({rc2}))", "f")
            if op == "-":
                return (f"(({lc2}) - ({rc2}))", "f")
            if op == "*":
                return (f"(({lc2}) * ({rc2}))", "f")
            if op == "//":
                return (f"cpy_fdiv_d({lc2}, {rc2})", "f")
            if op == "%":
                return (f"cpy_mod_d({lc2}, {rc2})", "f")
        else:
            lc2 = cast_c(lc, lt, "i")
            rc2 = cast_c(rc, rt, "i")
            if op == "+":
                return (f"cpy_add_i({lc2}, {rc2})", "i")
            if op == "-":
                return (f"cpy_sub_i({lc2}, {rc2})", "i")
            if op == "*":
                return (f"cpy_mul_i({lc2}, {rc2})", "i")
            if op == "//":
                return (f"cpy_fdiv_i({lc2}, {rc2})", "i")
            if op == "%":
                return (f"cpy_mod_i({lc2}, {rc2})", "i")
        self.err(f"internal: unsupported fast binop {op!r}")

    def gen_call_fast(self, n, fn):
        fnode = n["fn"]
        if fnode["kind"] != "name" or fnode["id"] not in self.functions:
            self.err("internal: fast codegen hit an unsupported call (TypeInfer should have excluded this)")
        callee = fnode["id"]
        if not self.tinfer.eligible.get(callee, False):
            self.err(f"internal: fast function calls non-fast '{callee}' (compiler bug)")
        cparams = self.tinfer.param_t[callee]
        if len(n["args"]) != len(cparams):
            self.err(f"internal: arity mismatch calling '{callee}' in fast codegen")
        args_c = []
        i = 0
        while i < len(cparams):
            ac, at = self.gen_expr_fast(n["args"][i], fn)
            if not call_arg_ok(at, cparams[i]):
                self.err(f"internal: argument type mismatch calling '{callee}' in fast codegen")
            args_c.append(cast_c(ac, at, cparams[i]))
            i += 1
        return (f"{c_name(callee)}__fast({', '.join(args_c)})", self.tinfer.ret_t[callee])

    def fast_call_matches(self, n, fn):
        """True iff this call can go through the direct fast-to-fast path
        (gen_call_fast) -- i.e. is exactly what gen_call_fast itself
        requires. Used by gen_stmt_fast to prefer that (fully unboxed,
        fastest) path over the boxed-call fallback whenever it applies;
        the boxed fallback is only for calls that don't qualify here."""
        fnode = n["fn"]
        if n.get("kwargs"):
            return False  # no positional-slot binding for kwargs in the fast tier (see call_type's matching guard)
        if fnode["kind"] != "name" or fnode["id"] not in self.functions:
            return False
        callee = fnode["id"]
        if not self.tinfer.eligible.get(callee, False):
            return False
        cparams = self.tinfer.param_t[callee]
        if len(n["args"]) != len(cparams):
            return False
        i = 0
        while i < len(cparams):
            if not call_arg_ok(self.tinfer.expr_type(fn, n["args"][i]), cparams[i]):
                return False
            i += 1
        return True

    def gen_fast_arg_boxed(self, n, fn):
        """Mirror of TypeInfer.arg_representable at codegen time: produce a
        C expression yielding a boxed Value for argument `n` of a boxable
        call made from inside a fast function. Recurses for a nested
        boxable call (print(abs(x)), print(helper(x, y)), ...); otherwise
        falls back to evaluating it in the fast domain and boxing the
        result. Keep in sync with TypeInfer.arg_representable/stmt_call_boxable,
        which already proved this will succeed for every node it's called on."""
        if n["kind"] == "call" and n["fn"]["kind"] == "name" and not self.fast_call_matches(n, fn):
            name = n["fn"]["id"]
            args_c = [self.gen_fast_arg_boxed(a, fn) for a in n["args"]]
            argv = ", ".join(args_c)
            if name in self.functions:
                return f"{c_name(name)}({argv})"
            self.used_builtins.add(name)
            if args_c:
                return f'call_value({ext_c_name(name)}, (Value[]){{{argv}}}, {len(args_c)}, NULL)'
            return f"call_value({ext_c_name(name)}, NULL, 0, NULL)"
        ac, at = self.gen_expr_fast(n, fn)
        return box_result(ac, at)

    def gen_boxed_call_stmt_fast(self, n, fn):
        """A call in statement position (result discarded) inside a fast
        function body, to something that isn't itself fast-callable --
        typically a builtin like print(), or a sibling function that
        doesn't (yet) qualify for its own fast tier. TypeInfer.stmt_call_boxable
        already confirmed every argument is representable, so: evaluate/box
        each (gen_fast_arg_boxed), and make the call exactly the way the
        boxed tier would -- the result (if any) is discarded, so nothing
        needs to come back into the fast/unboxed domain."""
        name = n["fn"]["id"]
        args_c = [self.gen_fast_arg_boxed(a, fn) for a in n["args"]]
        argv = ", ".join(args_c)
        if name in self.functions:
            arity = self.functions[name]
            if len(args_c) != arity:
                self.err(f"{name}() takes {arity} argument{'s' if arity != 1 else ''}, {len(args_c)} given")
            self.emit(f"(void)({c_name(name)}({argv}));")
            return
        self.used_builtins.add(name)
        if args_c:
            self.emit(f'(void)(call_value({ext_c_name(name)}, (Value[]){{{argv}}}, {len(args_c)}, NULL));')
        else:
            self.emit(f"(void)(call_value({ext_c_name(name)}, NULL, 0, NULL));")

    def gen_stmt_fast(self, s, fn):
        k = s["kind"]
        if k == "exprstmt":
            v = s["value"]
            if v["kind"] == "call" and self.tinfer.stmt_call_boxable(fn, v) and not self.fast_call_matches(v, fn):
                self.gen_boxed_call_stmt_fast(v, fn)
                return
            code, _t = self.gen_expr_fast(v, fn)
            self.emit(f"(void)({code});")
            return
        if k == "assign":
            tname = s["target"]["id"]
            tt = self.tinfer.var_type(fn, tname)
            code, at = self.gen_expr_fast(s["value"], fn)
            self.emit(f"{c_name(tname)} = {cast_c(code, at, tt)};")
            return
        if k == "augassign":
            tname = s["target"]["id"]
            tt = self.tinfer.var_type(fn, tname)
            if s["op"] == "**":
                rc, rt = self.gen_expr_fast(s["value"], fn)
                if tt == "f" or rt == "f":
                    code, rest_t = (f"cpy_pow_f({cast_c(c_name(tname), tt, 'f')}, {cast_c(rc, rt, 'f')})", "f")
                else:
                    code, rest_t = (f"cpy_pow_i({cast_c(c_name(tname), tt, 'i')}, {cast_c(rc, rt, 'i')})", "i")
            else:
                rc, rt = self.gen_expr_fast(s["value"], fn)
                code, rest_t = self.gen_binop_fast(s["op"], c_name(tname), tt, rc, rt)
            self.emit(f"{c_name(tname)} = {cast_c(code, rest_t, tt)};")
            return
        if k == "if":
            cond, _t = self.gen_expr_fast(s["cond"], fn)
            self.emit(f"if (({cond}) != 0) {{")
            self.indent += 1
            self.gen_body_fast(s["body"], fn)
            self.indent -= 1
            if s["orelse"]:
                self.emit("} else {")
                self.indent += 1
                self.gen_body_fast(s["orelse"], fn)
                self.indent -= 1
            self.emit("}")
            return
        if k == "while":
            cond, _t = self.gen_expr_fast(s["cond"], fn)
            self.emit(f"while (({cond}) != 0) {{")
            self.indent += 1
            self.gen_body_fast(s["body"], fn)
            self.indent -= 1
            self.emit("}")
            return
        if k == "for":
            self.gen_for_fast(s, fn)
            return
        if k == "return":
            rtype = self.tinfer.ret_t[fn]
            code, at = self.gen_expr_fast(s["value"], fn)
            self.emit(f"return {cast_c(code, at, rtype)};")
            return
        if k == "break":
            self.emit("break;")
            return
        if k == "continue":
            self.emit("continue;")
            return
        if k == "pass":
            return
        self.err(f"internal: unsupported fast statement kind {k!r}")

    def gen_body_fast(self, stmts, fn):
        for s in stmts:
            self.gen_stmt_fast(s, fn)

    def gen_for_fast(self, s, fn):
        target = s["target"]
        it = s["iter"]
        rargs = it["args"]
        if len(rargs) == 1:
            lo_c = "0LL"
            hi_c = self.gen_expr_fast(rargs[0], fn)[0]
            step_c = "1LL"
        elif len(rargs) == 2:
            lo_c = self.gen_expr_fast(rargs[0], fn)[0]
            hi_c = self.gen_expr_fast(rargs[1], fn)[0]
            step_c = "1LL"
        else:
            lo_c = self.gen_expr_fast(rargs[0], fn)[0]
            hi_c = self.gen_expr_fast(rargs[1], fn)[0]
            step_c = self.gen_expr_fast(rargs[2], fn)[0]
        li, hv, st = self.new_tmp(), self.new_tmp(), self.new_tmp()
        self.emit("{")
        self.indent += 1
        self.emit(f"int64_t {li} = ({lo_c}), {hv} = ({hi_c}), {st} = ({step_c}), _fi;")
        self.emit(f"for (_fi = {li}; {st} > 0 ? _fi < {hv} : _fi > {hv}; _fi += {st}) {{")
        self.indent += 1
        self.emit(f"{c_name(target)} = _fi;")
        self.gen_body_fast(s["body"], fn)
        self.indent -= 1
        self.emit("}")
        self.indent -= 1
        self.emit("}")

    def gen_function_fast(self, d):
        name = d["name"]
        params = d["params"]
        ptypes = self.tinfer.param_t[name]
        rtype = self.tinfer.ret_t[name]
        sig_parts = []
        i = 0
        while i < len(params):
            sig_parts.append(f"{ctype_of(ptypes[i])} {c_name(params[i])}")
            i += 1
        sig = ", ".join(sig_parts) or "void"
        self.emit_top(f"static {ctype_of(rtype)} {c_name(name)}__fast({sig}) {{")
        self.indent = 1
        # Deliberately NO frame tracking here (see gen_function's Frame for
        # the boxed tier): measured cost on fib(35) was +25% just from the
        # Frame struct writes, +75% once the cleanup-attribute pop needed
        # to handle it correctly on every return path was added -- for a
        # tier that exists specifically to get AOT numeric code close to
        # raw C speed, that's not an acceptable trade for traceback
        # completeness. An exception raised inside a chain of fast calls
        # still gets the correct class/message and the outermost
        # boxed/module frame that started the fast chain; it just won't
        # show the fast functions themselves. Known, accepted limitation.
        locals_ = set()
        self.collect_assigned(d["body"], locals_)
        locals_ -= set(params)
        for loc in sorted(locals_):
            t = self.tinfer.locals_t[name].get(loc, "i")
            default_lit = "0.0" if t == "f" else "0"
            self.emit(f"{ctype_of(t)} {c_name(loc)} = {default_lit};")
        self.gen_body_fast(d["body"], name)
        # Unreachable given TypeInfer's always-returns check, but keeps the
        # C compiler happy about all control paths returning a value.
        self.emit(f"return ({ctype_of(rtype)})0;")
        self.emit_top("}")
        self.emit_top("")

    # ------------------------------------------------------------ names
    def collect_assigned(self, stmts, names):
        for s in stmts:
            k = s["kind"]
            if k == "assign" and s["target"]["kind"] == "name":
                names.add(s["target"]["id"])
            elif k == "for":
                if not isinstance(s["target"], list):
                    names.add(s["target"])
                self.collect_assigned(s["body"], names)
            elif k == "if":
                self.collect_assigned(s["body"], names)
                self.collect_assigned(s["orelse"], names)
            elif k == "while":
                self.collect_assigned(s["body"], names)

    # -------------------------------------------------------- top level
    def gen_function(self, n):
        name = n["name"]
        self.cur_line = n.get("line", self.cur_line)
        if n["defaults"]:
            self.err(f"AOT v1 does not support default arguments ({name})")
        params = n["params"]
        for p in params:
            if p in self.modules:
                self.err(f"parameter '{p}' shadows the imported module of the same name")
        locals_ = set()
        self.collect_assigned(n["body"], locals_)
        locals_ -= set(params)
        sig = ", ".join(f"Value {c_name(p)}" for p in params) or "void"
        self.emit_top(f"static Value {c_name(name)}({sig}) {{")
        self.indent = 1
        self.cur_fn = name
        fline = n.get("line", 0)
        self.emit(f'Frame fr __attribute__((cleanup(cpy_frame_pop))) = {{ g_frame, NULL, NULL, {c_str_lit(self.filename)}, {c_str_lit(name)}, {fline}, V_none() }};')
        self.emit("g_frame = &fr;")
        for loc in sorted(locals_):
            self.emit(f"Value {c_name(loc)} = V_none();")
        self.gen_body(n["body"])
        self.emit("return V_none();")
        self.cur_fn = None
        self.emit_top("}")
        self.emit_top("")

    def emit_top(self, line):
        self.out.append(line)

    def compile_module(self, prog, filename="<aot>"):
        self.out = []
        self.filename = filename
        imports = [s for s in prog if s["kind"] == "import"]
        all_defs = [s for s in prog if s["kind"] == "def"]
        top_stmts = [s for s in prog if s["kind"] not in ("def", "import")]

        for imp in imports:
            for mod_name, alias in imp["modules"]:
                local = alias or mod_name.split(".")[0]
                self.modules[local] = mod_name

        # Reachability: a top-level def or class that nothing reachable from
        # the statements which actually RUN (top_stmts -- always executed,
        # in order, so always required) ever refers to, directly or
        # transitively, has zero effect on the program's behavior whether
        # we compile it or skip it. So: skip it -- an unrelated class or
        # helper function elsewhere in the file (using strings, try/except,
        # whatever) no longer has to be AOT-compilable just because it's
        # *present*, only if something that actually runs can reach it.
        # (A top-level class/def that IS reachable still correctly fails to
        # compile exactly as before if it needs something AOT can't do --
        # this only removes failures for code that was never going to run.)
        defs_by_name = {}
        for d in all_defs:
            defs_by_name[d["name"]] = d
        classes_by_name = {}
        for s in prog:
            if s["kind"] == "class":
                classes_by_name[s["name"]] = s
        reachable = set()

        def mark_reachable(nm):
            if nm in reachable:
                return
            reachable.add(nm)
            names = set()
            if nm in defs_by_name:
                collect_names(defs_by_name[nm]["body"], names)
            elif nm in classes_by_name:
                collect_names(classes_by_name[nm]["body"], names)
            else:
                return
            for n2 in names:
                mark_reachable(n2)

        seed_names = set()
        collect_names(top_stmts, seed_names)
        for nm in seed_names:
            mark_reachable(nm)
        defs = [d for d in all_defs if d["name"] in reachable]
        top_stmts = [s for s in top_stmts if not (s["kind"] == "class" and s["name"] not in reachable)]

        for d in defs:
            self.cur_line = d.get("line", 0)
            if d["defaults"]:
                self.err(f"AOT v1 does not support default arguments ({d['name']})")
            if d["name"] in self.modules:
                self.err(f"function '{d['name']}' has the same name as an imported module")
            self.functions[d["name"]] = len(d["params"])

        # Whole-program numeric type inference for the unboxed fast tier
        # (see the big comment above the TypeInfer class). This runs over
        # the same AST the boxed codegen below processes and never raises
        # -- it just determines which functions (if any) additionally
        # qualify for a `<name>__fast` variant, and which call sites can
        # be redirected to use it.
        self.tinfer = TypeInfer(prog)

        # Forward-declare every compiled function first, so they can call
        # each other (including mutual/forward recursion) regardless of
        # source order, then emit each body. Fast (unboxed) variants get
        # their own forward declarations too, for the same reason.
        for name in self.functions:
            arity = self.functions[name]
            sig = ", ".join("Value" for _ in range(arity)) or "void"
            self.emit_top(f"static Value {c_name(name)}({sig});")
        for name in self.functions:
            if self.tinfer.eligible.get(name, False):
                ptypes = self.tinfer.param_t[name]
                rtype = self.tinfer.ret_t[name]
                fsig = ", ".join(ctype_of(t) for t in ptypes) or "void"
                self.emit_top(f"static {ctype_of(rtype)} {c_name(name)}__fast({fsig});")
        if self.functions:
            self.emit_top("")

        for d in defs:
            self.gen_function(d)
        for d in defs:
            if self.tinfer.eligible.get(d["name"], False):
                self.gen_function_fast(d)

        self.emit_top("int main(int argc, char **argv) {")
        self.indent = 1
        self.emit("(void)argc; (void)argv;")
        self.emit("interp_init();")
        self.emit("Handler h; h.prev = NULL; h.frame = NULL; h.depth = 0; h.exc = V_none();")
        self.emit("g_handler = &h;")
        self.emit("if (setjmp(h.jb) == 0) {")
        self.indent = 2
        for mod_name in sorted(set(self.modules.values())):
            self.emit(f'{mod_c_name(mod_name)} = import_module(INTERN("{mod_name}"));')
        # used_builtins is only fully known once every statement (in every
        # function body above, and every top-level statement below) has been
        # visited -- so its runtime-init lines are inserted further down,
        # AFTER gen_body(top_stmts) has run, by splicing them into self.out.
        builtins_init_at = len(self.out)
        self.emit(f'Frame fr __attribute__((cleanup(cpy_frame_pop))) = {{ NULL, NULL, NULL, {c_str_lit(self.filename)}, "<module>", 0, V_none() }};')
        self.emit("g_frame = &fr;")
        top_locals = set()
        self.collect_assigned(top_stmts, top_locals)
        for loc in sorted(top_locals):
            self.emit(f"Value {c_name(loc)} = V_none();")
        self.gen_body(top_stmts)
        self.emit("g_handler = NULL;")
        self.emit("return 0;")
        self.indent = 1
        self.emit("}")
        # An uncaught exception longjmps straight here, skipping every
        # cleanup-attribute Frame pop between the throw site and main() --
        # that's fine (and exactly what the tree-walking interpreter's own
        # run_guarded() does too): report_uncaught() only needs the
        # traceback text capture_tb() already built at the moment of the
        # throw (while the frame chain was still intact), not live frames.
        self.emit("g_handler = NULL; g_frame = NULL; g_depth = 0;")
        self.emit("report_uncaught(h.exc);")
        self.emit("return 1;")
        self.indent = 0
        self.emit_top("}")

        init_lines = [f'        {ext_c_name(b)} = *dict_find_cstr(g_builtins, "{b}");' for b in sorted(self.used_builtins)]
        self.out[builtins_init_at:builtins_init_at] = init_lines

        header = ['/* generated by selfhost/aotc.cpy - do not edit */', '#include "cpy.h"', ""]
        for mod_name in sorted(set(self.modules.values())):
            header.append(f"static Value {mod_c_name(mod_name)};")
        for b in sorted(self.used_builtins):
            header.append(f"static Value {ext_c_name(b)};")
        header.append("")

        return "\n".join(header + self.out) + "\n"


def compile_source(src, filename="<aot>"):
    from parser import parse
    prog = parse(src)
    return Gen().compile_module(prog, filename)
