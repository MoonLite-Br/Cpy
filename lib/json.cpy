# lib/json.cpy - a small, dependency-free JSON encoder/decoder.

_WS = " \t\n\r"


class JSONDecodeError(ValueError):
    pass


def _skip_ws(s, i):
    n = len(s)
    while i < n and s[i] in _WS:
        i += 1
    return i


def _parse_string(s, i):
    assert s[i] == '"'
    i += 1
    out = []
    n = len(s)
    while True:
        if i >= n:
            raise JSONDecodeError("unterminated string")
        c = s[i]
        if c == '"':
            return "".join(out), i + 1
        if c == "\\":
            i += 1
            e = s[i]
            if e == '"':
                out.append('"')
            elif e == "\\":
                out.append("\\")
            elif e == "/":
                out.append("/")
            elif e == "n":
                out.append("\n")
            elif e == "t":
                out.append("\t")
            elif e == "r":
                out.append("\r")
            elif e == "b":
                out.append("\b")
            elif e == "f":
                out.append("\f")
            elif e == "u":
                code = int(s[i + 1:i + 5], 16)
                out.append(chr(code))
                i += 4
            else:
                raise JSONDecodeError(f"invalid escape \\{e}")
            i += 1
            continue
        out.append(c)
        i += 1


def _parse_number(s, i):
    n = len(s)
    j = i
    if j < n and s[j] == "-":
        j += 1
    while j < n and s[j].isdigit():
        j += 1
    is_float = False
    if j < n and s[j] == ".":
        is_float = True
        j += 1
        while j < n and s[j].isdigit():
            j += 1
    if j < n and (s[j] == "e" or s[j] == "E"):
        is_float = True
        j += 1
        if j < n and (s[j] == "+" or s[j] == "-"):
            j += 1
        while j < n and s[j].isdigit():
            j += 1
    text = s[i:j]
    if is_float:
        return float(text), j
    return int(text), j


def _parse_value(s, i):
    i = _skip_ws(s, i)
    if i >= len(s):
        raise JSONDecodeError("unexpected end of input")
    c = s[i]
    if c == '"':
        return _parse_string(s, i)
    if c == "{":
        return _parse_object(s, i)
    if c == "[":
        return _parse_array(s, i)
    if s[i:i + 4] == "true":
        return True, i + 4
    if s[i:i + 5] == "false":
        return False, i + 5
    if s[i:i + 4] == "null":
        return None, i + 4
    if c == "-" or c.isdigit():
        return _parse_number(s, i)
    raise JSONDecodeError(f"unexpected character {c!r} at position {i}")


def _parse_object(s, i):
    obj = {}
    i += 1
    i = _skip_ws(s, i)
    if i < len(s) and s[i] == "}":
        return obj, i + 1
    while True:
        i = _skip_ws(s, i)
        if s[i] != '"':
            raise JSONDecodeError("expected string key")
        key, i = _parse_string(s, i)
        i = _skip_ws(s, i)
        if s[i] != ":":
            raise JSONDecodeError("expected ':'")
        i += 1
        value, i = _parse_value(s, i)
        obj[key] = value
        i = _skip_ws(s, i)
        if i >= len(s):
            raise JSONDecodeError("unterminated object")
        if s[i] == ",":
            i += 1
            continue
        if s[i] == "}":
            return obj, i + 1
        raise JSONDecodeError(f"expected ',' or '}}' at position {i}")


def _parse_array(s, i):
    arr = []
    i += 1
    i = _skip_ws(s, i)
    if i < len(s) and s[i] == "]":
        return arr, i + 1
    while True:
        value, i = _parse_value(s, i)
        arr.append(value)
        i = _skip_ws(s, i)
        if i >= len(s):
            raise JSONDecodeError("unterminated array")
        if s[i] == ",":
            i += 1
            continue
        if s[i] == "]":
            return arr, i + 1
        raise JSONDecodeError(f"expected ',' or ']' at position {i}")


def loads(s):
    value, i = _parse_value(s, 0)
    i = _skip_ws(s, i)
    if i != len(s):
        raise JSONDecodeError(f"extra data at position {i}")
    return value


def load(f):
    return loads(f.read())


_ESCAPES = {'"': '\\"', "\\": "\\\\", "\n": "\\n", "\t": "\\t", "\r": "\\r", "\b": "\\b", "\f": "\\f"}


def _dump_string(s, out):
    out.append('"')
    for c in s:
        if c in _ESCAPES:
            out.append(_ESCAPES[c])
        elif ord(c) < 0x20:
            out.append("\\u%04x" % ord(c))
        else:
            out.append(c)
    out.append('"')


def _dump(v, out, indent, level, sort_keys):
    if v is None:
        out.append("null")
    elif v is True:
        out.append("true")
    elif v is False:
        out.append("false")
    elif isinstance(v, int):
        out.append(str(v))
    elif isinstance(v, float):
        out.append(str(v))
    elif isinstance(v, str):
        _dump_string(v, out)
    elif isinstance(v, (list, tuple)):
        if not v:
            out.append("[]")
            return
        nl, pad, pad_close, sep = _pads(indent, level)
        out.append("[" + nl)
        for i in range(len(v)):
            out.append(pad)
            _dump(v[i], out, indent, level + 1, sort_keys)
            out.append(sep + nl if i < len(v) - 1 else nl)
        out.append(pad_close + "]")
    elif isinstance(v, dict):
        if not v:
            out.append("{}")
            return
        nl, pad, pad_close, sep = _pads(indent, level)
        keys = sorted(v.keys()) if sort_keys else list(v.keys())
        out.append("{" + nl)
        for i in range(len(keys)):
            k = keys[i]
            out.append(pad)
            _dump_string(str(k), out)
            out.append(": ")
            _dump(v[k], out, indent, level + 1, sort_keys)
            out.append(sep + nl if i < len(keys) - 1 else nl)
        out.append(pad_close + "}")
    else:
        raise TypeError(f"Object of type {type(v).__name__} is not JSON serializable")


def _pads(indent, level):
    if indent is None:
        return "", "", "", ", "
    nl = "\n"
    pad = " " * (indent * (level + 1))
    pad_close = " " * (indent * level)
    return nl, pad, pad_close, ","


def dumps(v, indent=None, sort_keys=False):
    out = []
    _dump(v, out, indent, 0, sort_keys)
    return "".join(out)


def dump(v, f, indent=None, sort_keys=False):
    f.write(dumps(v, indent=indent, sort_keys=sort_keys))
