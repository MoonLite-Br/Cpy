# selfhost/parser.cpy - recursive-descent parser -> AST (plain dicts) for the
# self-hosted subset. Run by the real cpy interpreter.

from lexer import tokenize, LexError


class ParseError(Exception):
    pass


def decode_str_escapes(s):
    """Decode backslash escapes in a literal text segment -- used for the
    literal portions of an f-string, which the lexer (FSTRING token) hands
    over raw/undecoded (see lexer.cpy's f-string branch for why)."""
    out = []
    i = 0
    n = len(s)
    while i < n:
        ch = s[i]
        if ch == "\\" and i + 1 < n:
            nxt = s[i + 1]
            if nxt == "n":
                out.append("\n")
            elif nxt == "t":
                out.append("\t")
            elif nxt == "\\":
                out.append("\\")
            elif nxt == "'":
                out.append("'")
            elif nxt == '"':
                out.append('"')
            elif nxt == "0":
                out.append("\0")
            else:
                out.append("\\")
                out.append(nxt)
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def node(kind, **fields):
    fields["kind"] = kind
    return fields


class Parser:
    def __init__(self, tokens):
        self.toks = tokens
        self.pos = 0

    def peek(self):
        return self.toks[self.pos]

    def at(self, kind):
        return self.toks[self.pos].kind == kind

    def advance(self):
        t = self.toks[self.pos]
        self.pos += 1
        return t

    def expect(self, kind):
        if not self.at(kind):
            t = self.peek()
            raise ParseError(f"line {t.line}: expected {kind}, got {t.kind} ({t.value!r})")
        return self.advance()

    def accept(self, kind):
        if self.at(kind):
            return self.advance()
        return None

    # ---------------------------------------------------------- program
    def parse_program(self):
        stmts = []
        while not self.at("EOF"):
            if self.accept("NEWLINE"):
                continue
            stmts.append(self.parse_stmt())
        return stmts

    def parse_block(self):
        self.expect(":")
        self.expect("NEWLINE")
        self.expect("INDENT")
        stmts = []
        while not self.at("DEDENT") and not self.at("EOF"):
            if self.accept("NEWLINE"):
                continue
            stmts.append(self.parse_stmt())
        self.expect("DEDENT")
        return stmts

    # -------------------------------------------------------- statements
    def parse_stmt(self):
        t = self.peek()
        if t.kind == "def":
            return self.parse_def()
        if t.kind == "class":
            return self.parse_class()
        if t.kind == "if":
            return self.parse_if()
        if t.kind == "while":
            return self.parse_while()
        if t.kind == "for":
            return self.parse_for()
        if t.kind == "return":
            self.advance()
            value = None
            if not self.at("NEWLINE"):
                value = self.parse_expr()
            self.expect("NEWLINE")
            return node("return", value=value, line=t.line)
        if t.kind == "break":
            self.advance()
            self.expect("NEWLINE")
            return node("break", line=t.line)
        if t.kind == "continue":
            self.advance()
            self.expect("NEWLINE")
            return node("continue", line=t.line)
        if t.kind == "pass":
            self.advance()
            self.expect("NEWLINE")
            return node("pass", line=t.line)
        if t.kind == "global":
            self.advance()
            names = [self.expect("NAME").value]
            while self.accept(","):
                names.append(self.expect("NAME").value)
            self.expect("NEWLINE")
            return node("global", names=names, line=t.line)
        if t.kind == "nonlocal":
            self.advance()
            names = [self.expect("NAME").value]
            while self.accept(","):
                names.append(self.expect("NAME").value)
            self.expect("NEWLINE")
            return node("nonlocal", names=names, line=t.line)
        if t.kind == "raise":
            self.advance()
            exc = None
            cause = None
            if not self.at("NEWLINE"):
                exc = self.parse_expr()
                if self.accept("from"):
                    cause = self.parse_expr()
            self.expect("NEWLINE")
            return node("raise", exc=exc, cause=cause, line=t.line)
        if t.kind == "assert":
            self.advance()
            test = self.parse_expr()
            msg = None
            if self.accept(","):
                msg = self.parse_expr()
            self.expect("NEWLINE")
            return node("assert", test=test, msg=msg, line=t.line)
        if t.kind == "del":
            self.advance()
            targets = [self.parse_expr()]
            while self.accept(","):
                targets.append(self.parse_expr())
            self.expect("NEWLINE")
            return node("del", targets=targets, line=t.line)
        if t.kind == "from":
            self.advance()
            mod_name = self.expect("NAME").value
            while self.accept("."):
                mod_name = mod_name + "." + self.expect("NAME").value
            self.expect("import")
            names = []
            paren = self.accept("(")
            if self.accept("*"):
                names.append(("*", None))
            else:
                while True:
                    nm = self.expect("NAME").value
                    alias = None
                    if self.accept("as"):
                        alias = self.expect("NAME").value
                    names.append((nm, alias))
                    if not self.accept(","):
                        break
                    if paren and self.at(")"):
                        break
            if paren:
                self.expect(")")
            self.expect("NEWLINE")
            return node("from_import", module=mod_name, names=names, line=t.line)
        if t.kind == "try":
            return self.parse_try()
        if t.kind == "with":
            return self.parse_with()
        if t.kind == "async":
            self.advance()
            if not self.at("def"):
                raise ParseError(f"line {t.line}: expected 'def' after 'async'")
            d = self.parse_def()
            d["is_async"] = True
            return d
        if t.kind == "import":
            self.advance()
            modules = []
            while True:
                mod_name = self.expect("NAME").value
                while self.accept("."):
                    mod_name = mod_name + "." + self.expect("NAME").value
                alias = None
                if self.at("as"):
                    self.advance()
                    alias = self.expect("NAME").value
                modules.append((mod_name, alias))
                if not self.accept(","):
                    break
            self.expect("NEWLINE")
            return node("import", modules=modules, line=t.line)
        return self.parse_simple()

    def parse_def(self):
        line = self.advance().line
        name = self.expect("NAME").value
        self.expect("(")
        params = []
        defaults = []
        while not self.at(")"):
            pname = self.expect("NAME").value
            params.append(pname)
            if self.accept(":"):
                self.parse_expr()  # parameter type annotation -- parsed, not used by AOT
            if self.accept("="):
                defaults.append((pname, self.parse_expr()))
            if not self.accept(","):
                break
        self.expect(")")
        if self.accept("->"):
            self.parse_expr()  # return type annotation -- parsed, not used by AOT
        body = self.parse_block()
        return node("def", name=name, params=params, defaults=defaults, body=body, line=line)

    def parse_class(self):
        line = self.advance().line
        name = self.expect("NAME").value
        base = None
        if self.accept("("):
            if not self.at(")"):
                base = self.expect("NAME").value
            self.expect(")")
        body = self.parse_block()
        return node("class", name=name, base=base, body=body, line=line)

    def parse_if(self):
        line = self.advance().line
        cond = self.parse_expr()
        body = self.parse_block()
        orelse = []
        if self.at("elif"):
            orelse = [self.parse_if_elif()]
        elif self.accept("else"):
            orelse = self.parse_block()
        return node("if", cond=cond, body=body, orelse=orelse, line=line)

    def parse_if_elif(self):
        line = self.advance().line   # consumes 'elif' like 'if'
        cond = self.parse_expr()
        body = self.parse_block()
        orelse = []
        if self.at("elif"):
            orelse = [self.parse_if_elif()]
        elif self.accept("else"):
            orelse = self.parse_block()
        return node("if", cond=cond, body=body, orelse=orelse, line=line)

    def parse_while(self):
        line = self.advance().line
        cond = self.parse_expr()
        body = self.parse_block()
        return node("while", cond=cond, body=body, line=line)

    def parse_for(self):
        line = self.advance().line
        names = [self.expect("NAME").value]
        while self.accept(","):
            names.append(self.expect("NAME").value)
        target = names[0] if len(names) == 1 else names
        self.expect("in")
        it = self.parse_expr()
        body = self.parse_block()
        return node("for", target=target, iter=it, body=body, line=line)

    def parse_try(self):
        line = self.advance().line
        body = self.parse_block()
        handlers = []
        while self.at("except"):
            hline = self.advance().line
            exc_type = None
            exc_name = None
            if not self.at(":"):
                exc_type = self.parse_expr()
                if self.accept("as"):
                    exc_name = self.expect("NAME").value
            hbody = self.parse_block()
            handlers.append(node("except", exc_type=exc_type, name=exc_name, body=hbody, line=hline))
        orelse = []
        if handlers and self.at("else"):
            self.advance()
            orelse = self.parse_block()
        finalbody = []
        if self.accept("finally"):
            finalbody = self.parse_block()
        if not handlers and not finalbody:
            raise ParseError(f"line {line}: expected 'except' or 'finally' block")
        return node("try", body=body, handlers=handlers, orelse=orelse, finalbody=finalbody, line=line)

    def parse_with(self):
        line = self.advance().line
        items = []
        while True:
            ctx = self.parse_expr()
            asname = None
            if self.accept("as"):
                asname = self.expect("NAME").value
            items.append((ctx, asname))
            if not self.accept(","):
                break
        body = self.parse_block()
        return node("with", items=items, body=body, line=line)

    def parse_simple(self):
        line = self.peek().line
        first = self.parse_expr()
        if self.accept(":"):
            # Variable annotation (`x: int` or `x: int = 1`). AOT doesn't
            # use the type itself -- parse and discard it, same as a
            # plain `cpy` script would just ignore it at runtime too.
            self.parse_expr()
            if self.accept("="):
                value = self.parse_expr()
                self.expect("NEWLINE")
                return node("assign", target=first, value=value, line=line)
            self.expect("NEWLINE")
            return node("pass", line=line)  # bare `x: int` declares, doesn't assign
        if self.at("=") or self.at("+=") or self.at("-=") or self.at("*=") or self.at("/=") or self.at("%=") or self.at("//=") or self.at("**="):
            op = self.advance().kind
            value = self.parse_expr()
            self.expect("NEWLINE")
            if op == "=":
                return node("assign", target=first, value=value, line=line)
            return node("augassign", target=first, op=op[:-1], value=value, line=line)
        self.expect("NEWLINE")
        return node("exprstmt", value=first, line=line)

    # -------------------------------------------------------- expressions
    def parse_expr(self):
        if self.at("lambda"):
            return self.parse_lambda()
        if self.at("yield"):
            return self.parse_yield()
        return self.parse_or()

    def parse_lambda(self):
        line = self.advance().line
        params = []
        defaults = []
        while not self.at(":"):
            pname = self.expect("NAME").value
            params.append(pname)
            if self.accept("="):
                defaults.append((pname, self.parse_or()))
            if not self.accept(","):
                break
        self.expect(":")
        body = self.parse_expr()
        return node("lambda", params=params, defaults=defaults, body=body, line=line)

    def parse_yield(self):
        line = self.advance().line
        if self.accept("from"):
            return node("yieldfrom", value=self.parse_or(), line=line)
        if self.at("NEWLINE") or self.at(")") or self.at("]") or self.at("}") or self.at(","):
            return node("yield", value=None, line=line)
        return node("yield", value=self.parse_or(), line=line)

    def parse_or(self):
        left = self.parse_and()
        while self.at("or"):
            self.advance()
            left = node("or", left=left, right=self.parse_and())
        return left

    def parse_and(self):
        left = self.parse_not()
        while self.at("and"):
            self.advance()
            left = node("and", left=left, right=self.parse_not())
        return left

    def parse_not(self):
        if self.at("not"):
            self.advance()
            return node("not", value=self.parse_not())
        return self.parse_cmp()

    CMP_OPS = {"==", "!=", "<", "<=", ">", ">=", "in"}

    def parse_cmp(self):
        left = self.parse_bitor()
        if self.at("is"):
            self.advance()
            negate = bool(self.accept("not"))
            right = self.parse_bitor()
            c = node("cmp", op="is", left=left, right=right)
            return node("not", value=c) if negate else c
        if self.at("not") and self.toks[self.pos + 1].kind == "in":
            self.advance()
            self.advance()
            right = self.parse_bitor()
            c = node("cmp", op="in", left=left, right=right)
            return node("not", value=c)
        if self.peek().kind in self.CMP_OPS:
            op = self.advance().kind
            right = self.parse_bitor()
            return node("cmp", op=op, left=left, right=right)
        return left

    def parse_bitor(self):
        return self.parse_add()

    def parse_add(self):
        left = self.parse_term()
        while self.peek().kind in ("+", "-"):
            op = self.advance().kind
            left = node("binop", op=op, left=left, right=self.parse_term())
        return left

    def parse_term(self):
        left = self.parse_factor()
        while self.peek().kind in ("*", "/", "//", "%"):
            op = self.advance().kind
            left = node("binop", op=op, left=left, right=self.parse_factor())
        return left

    def parse_factor(self):
        if self.at("await"):
            line = self.advance().line
            return node("await", value=self.parse_factor(), line=line)
        if self.peek().kind in ("+", "-"):
            op = self.advance().kind
            return node("unary", op=op, value=self.parse_factor())
        return self.parse_power()

    def parse_power(self):
        left = self.parse_postfix()
        if self.at("**"):
            self.advance()
            return node("binop", op="**", left=left, right=self.parse_factor())
        return left

    def parse_postfix(self):
        e = self.parse_atom()
        while True:
            if self.accept("("):
                args = []
                kwargs = []
                while not self.at(")"):
                    if self.at("NAME") and self.toks[self.pos + 1].kind == "=":
                        kwname = self.advance().value
                        self.advance()  # '='
                        kwargs.append((kwname, self.parse_expr()))
                    else:
                        args.append(self.parse_expr())
                    if not self.accept(","):
                        break
                self.expect(")")
                e = node("call", fn=e, args=args, kwargs=kwargs)
            elif self.accept("["):
                lo = None if self.at(":") else self.parse_expr()
                if self.accept(":"):
                    hi = None if self.at("]") else self.parse_expr()
                    self.expect("]")
                    e = node("slice", obj=e, lo=lo, hi=hi)
                else:
                    self.expect("]")
                    e = node("index", obj=e, idx=lo)
            elif self.accept("."):
                name = self.expect("NAME").value
                e = node("attr", obj=e, name=name)
            else:
                break
        return e

    def parse_fstring_token(self, raw, line):
        """Split an f-string's raw contents into literal-text and {expr}
        parts (mirroring src/parser.c's parse_fstring), recursively
        re-lexing+parsing each {expr} as an ordinary expression. Supports
        {expr}, {expr:spec}, {expr!conv}, {expr!conv:spec}; does not
        support the {expr=} debug-equals shorthand (rare enough in
        practice, and AOT can't use any of this anyway -- see codegen.cpy,
        which rejects "fstring" like every other string-valued node)."""
        parts = []
        lit = []
        i = 0
        n = len(raw)
        while i < n:
            ch = raw[i]
            if ch == "{" and i + 1 < n and raw[i + 1] == "{":
                lit.append("{")
                i += 2
                continue
            if ch == "}" and i + 1 < n and raw[i + 1] == "}":
                lit.append("}")
                i += 2
                continue
            if ch == "}":
                raise ParseError(f"line {line}: f-string: single '}}' is not allowed")
            if ch != "{":
                lit.append(ch)
                i += 1
                continue
            if lit:
                parts.append(node("const", value=decode_str_escapes("".join(lit)), line=line))
                lit = []
            j = i + 1
            depth = 0
            quote = None
            colon = -1
            bang = -1
            while j < n:
                d = raw[j]
                if quote:
                    if d == quote:
                        quote = None
                    j += 1
                    continue
                if d == "'" or d == '"':
                    quote = d
                elif d == "(" or d == "[" or d == "{":
                    depth += 1
                elif d == ")" or d == "]":
                    depth -= 1
                elif d == "}":
                    if depth == 0:
                        break
                    depth -= 1
                elif depth == 0 and d == ":" and colon < 0:
                    colon = j
                elif depth == 0 and d == "!" and colon < 0 and j + 1 < n and raw[j + 1] != "=":
                    bang = j
                j += 1
            if j >= n:
                raise ParseError(f"line {line}: f-string: expecting '}}'")
            eend = colon if colon >= 0 else j
            conv = None
            if bang >= 0 and (colon < 0 or bang < colon):
                conv = raw[bang + 1]
                eend = bang
            expr_src = raw[i + 1:eend]
            toks = tokenize(expr_src + "\n")
            sub = Parser(toks)
            value = sub.parse_expr()
            spec = raw[colon + 1:j] if colon >= 0 else None
            parts.append(node("fmt", value=value, conv=conv, spec=spec, line=line))
            i = j + 1
        if lit:
            parts.append(node("const", value=decode_str_escapes("".join(lit)), line=line))
        return node("fstring", parts=parts, line=line)

    def parse_atom(self):
        t = self.peek()
        if t.kind == "INT" or t.kind == "FLOAT":
            self.advance()
            return node("const", value=t.value)
        if t.kind == "STRING":
            self.advance()
            return node("const", value=t.value)
        if t.kind == "FSTRING":
            self.advance()
            return self.parse_fstring_token(t.value, t.line)
        if t.kind == "True":
            self.advance()
            return node("const", value=True)
        if t.kind == "False":
            self.advance()
            return node("const", value=False)
        if t.kind == "None":
            self.advance()
            return node("const", value=None)
        if t.kind == "NAME":
            self.advance()
            return node("name", id=t.value)
        if t.kind == "(":
            self.advance()
            e = self.parse_expr()
            self.expect(")")
            return e
        if t.kind == "[":
            self.advance()
            items = []
            while not self.at("]"):
                items.append(self.parse_expr())
                if not self.accept(","):
                    break
            self.expect("]")
            return node("list", items=items)
        if t.kind == "{":
            self.advance()
            keys = []
            values = []
            while not self.at("}"):
                k = self.parse_expr()
                self.expect(":")
                v = self.parse_expr()
                keys.append(k)
                values.append(v)
                if not self.accept(","):
                    break
            self.expect("}")
            return node("dict", keys=keys, values=values)
        raise ParseError(f"line {t.line}: unexpected token {t.kind} ({t.value!r})")


def parse(src):
    return Parser(tokenize(src)).parse_program()
