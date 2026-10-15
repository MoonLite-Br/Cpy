whitespace = " \t\n\r\x0b\x0c"
ascii_lowercase = "abcdefghijklmnopqrstuvwxyz"
ascii_uppercase = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
ascii_letters = ascii_lowercase + ascii_uppercase
digits = "0123456789"
hexdigits = "0123456789abcdefABCDEF"
octdigits = "01234567"
punctuation = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
printable = digits + ascii_letters + punctuation + whitespace

def capwords(s, sep=None):
    return (sep or " ").join(w.capitalize() for w in s.split(sep))

class Template:
    def __init__(self, template):
        self.template = template
    def substitute(self, mapping=None, **kws):
        d = dict(mapping) if mapping else {}
        d.update(kws)
        return self._sub(d, True)
    def safe_substitute(self, mapping=None, **kws):
        d = dict(mapping) if mapping else {}
        d.update(kws)
        return self._sub(d, False)
    def _sub(self, d, strict):
        t = self.template
        out = []
        i = 0
        n = len(t)
        while i < n:
            c = t[i]
            if c != "$":
                out.append(c)
                i += 1
                continue
            if i + 1 < n and t[i + 1] == "$":
                out.append("$")
                i += 2
                continue
            j = i + 1
            if j < n and t[j] == "{":
                k = t.find("}", j)
                name = t[j + 1:k]
                end = k + 1
            else:
                k = j
                while k < n and (t[k].isalnum() or t[k] == "_"):
                    k += 1
                name = t[j:k]
                end = k
            if name and name in d:
                out.append(str(d[name]))
            elif strict:
                raise KeyError(name)
            else:
                out.append(t[i:end])
            i = end
        return "".join(out)
