# selfhost/lexer.cpy - tokenizer for the self-hosted cpy-in-cpy interpreter.
# Written in cpy itself and run BY the real (C) cpy interpreter.

KEYWORDS = {
    "def", "return", "if", "elif", "else", "while", "for", "in",
    "break", "continue", "pass", "and", "or", "not",
    "True", "False", "None", "class", "global",
    "import", "as",
    "try", "except", "finally", "with", "raise", "assert", "del",
    "nonlocal", "from", "yield", "async", "await", "is", "lambda",
}

OPS3 = ["**=", "//="]
OPS2 = ["**", "//", "==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "%=", "->"]


class Token:
    def __init__(self, kind, value, line):
        self.kind = kind
        self.value = value
        self.line = line

    def __repr__(self):
        return f"Token({self.kind!r}, {self.value!r}, line={self.line})"


class LexError(Exception):
    pass


def is_id_start(c):
    return c.isalpha() or c == "_"


def is_id_char(c):
    return c.isalnum() or c == "_"


def tokenize(src):
    tokens = []
    indents = [0]
    i = 0
    n = len(src)
    line = 1
    depth = 0          # bracket nesting: suppress NEWLINE/INDENT while > 0
    at_line_start = True
    pending_newline = False

    while i < n:
        if at_line_start and depth == 0:
            col = 0
            j = i
            while j < n and (src[j] == " " or src[j] == "\t"):
                col += 8 if src[j] == "\t" else 1
                j += 1
            if j < n and src[j] == "#":
                while j < n and src[j] != "\n":
                    j += 1
            if j < n and src[j] == "\n":
                i = j + 1
                line += 1
                continue
            if j >= n:
                i = j
                break
            i = j
            if col > indents[-1]:
                indents.append(col)
                tokens.append(Token("INDENT", col, line))
            else:
                while col < indents[-1]:
                    indents.pop()
                    tokens.append(Token("DEDENT", col, line))
                if col != indents[-1]:
                    raise LexError(f"bad indent at line {line}")
            at_line_start = False

        c = src[i]
        if c == "\n":
            i += 1
            if depth > 0:
                line += 1
                continue
            if pending_newline:
                tokens.append(Token("NEWLINE", None, line))
            line += 1
            at_line_start = True
            pending_newline = False
            continue
        if c == " " or c == "\t" or c == "\r":
            i += 1
            continue
        if c == "#":
            while i < n and src[i] != "\n":
                i += 1
            continue
        pending_newline = True

        if (c == "f" or c == "F") and i + 1 < n and (src[i + 1] == '"' or src[i + 1] == "'"):
            # f-string: captured RAW (escapes undecoded, {expr} markers intact) --
            # splitting literal-text/`{expr}`/format-spec and decoding escapes in
            # the literal parts both happen later, in the parser (parse_fstring),
            # mirroring src/parser.c's own two-phase approach (lexer hands over
            # the raw text; the parser is what understands f-string structure).
            quote = src[i + 1]
            start_line = line
            j = i + 2
            raw_start = j
            while True:
                if j >= n:
                    raise LexError(f"unterminated f-string at line {start_line}")
                ch = src[j]
                if ch == "\\" and j + 1 < n:
                    j += 2
                    continue
                if ch == quote:
                    break
                if ch == "\n":
                    raise LexError(f"unterminated f-string at line {start_line}")
                j += 1
            tokens.append(Token("FSTRING", src[raw_start:j], start_line))
            i = j + 1
            continue

        if c == '"' or c == "'":
            quote = c
            start_line = line
            j = i + 1
            buf = []
            while True:
                if j >= n:
                    raise LexError(f"unterminated string at line {start_line}")
                ch = src[j]
                if ch == "\\" and j + 1 < n:
                    nxt = src[j + 1]
                    if nxt == "n":
                        buf.append("\n")
                    elif nxt == "t":
                        buf.append("\t")
                    elif nxt == "\\":
                        buf.append("\\")
                    elif nxt == "'":
                        buf.append("'")
                    elif nxt == '"':
                        buf.append('"')
                    elif nxt == "0":
                        buf.append("\0")
                    else:
                        buf.append("\\")
                        buf.append(nxt)
                    j += 2
                    continue
                if ch == quote:
                    j += 1
                    break
                if ch == "\n":
                    raise LexError(f"unterminated string at line {start_line}")
                buf.append(ch)
                j += 1
            tokens.append(Token("STRING", "".join(buf), start_line))
            i = j
            continue

        if c.isdigit() or (c == "." and i + 1 < n and src[i + 1].isdigit()):
            j = i
            is_float = False
            while j < n and src[j].isdigit():
                j += 1
            if j < n and src[j] == "." and j + 1 < n and src[j + 1].isdigit():
                is_float = True
                j += 1
                while j < n and src[j].isdigit():
                    j += 1
            if j < n and (src[j] == "e" or src[j] == "E"):
                k = j + 1
                if k < n and (src[k] == "+" or src[k] == "-"):
                    k += 1
                if k < n and src[k].isdigit():
                    is_float = True
                    j = k
                    while j < n and src[j].isdigit():
                        j += 1
            text = src[i:j]
            if is_float:
                tokens.append(Token("FLOAT", float(text), line))
            else:
                tokens.append(Token("INT", int(text), line))
            i = j
            continue

        if is_id_start(c):
            j = i
            while j < n and is_id_char(src[j]):
                j += 1
            text = src[i:j]
            if text in KEYWORDS:
                tokens.append(Token(text, text, line))
            else:
                tokens.append(Token("NAME", text, line))
            i = j
            continue

        matched = None
        for op in OPS3:
            if src[i:i + 3] == op:
                matched = op
                break
        if not matched:
            for op in OPS2:
                if src[i:i + 2] == op:
                    matched = op
                    break
        if matched:
            tokens.append(Token(matched, matched, line))
            i += len(matched)
            continue

        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth = depth - 1 if depth > 0 else 0
        if c in "+-*/%()[]{}:,.<>=":
            tokens.append(Token(c, c, line))
            i += 1
            continue

        raise LexError(f"unexpected character {c!r} at line {line}")

    if pending_newline:
        tokens.append(Token("NEWLINE", None, line))
    while len(indents) > 1:
        indents.pop()
        tokens.append(Token("DEDENT", 0, line))
    tokens.append(Token("EOF", None, line))
    return tokens
