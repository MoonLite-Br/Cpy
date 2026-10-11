defaultdict = defaultdict
Counter = Counter
OrderedDict = OrderedDict

class deque:
    def __init__(self, iterable=(), maxlen=None):
        self._d = list(iterable)
        self._h = 0
        self.maxlen = maxlen
        if maxlen is not None and len(self._d) > maxlen:
            self._d = self._d[len(self._d) - maxlen:]
    def _norm(self):
        if self._h:
            self._d = self._d[self._h:]
            self._h = 0
    def __len__(self):
        return len(self._d) - self._h
    def __bool__(self):
        return len(self._d) - self._h > 0
    def __iter__(self):
        return iter(self._d[self._h:])
    def __contains__(self, x):
        return x in self._d[self._h:]
    def _idx(self, i):
        n = len(self._d) - self._h
        if i < 0:
            i += n
        if i < 0 or i >= n:
            raise IndexError("deque index out of range")
        return self._h + i
    def __getitem__(self, i):
        return self._d[self._idx(i)]
    def __setitem__(self, i, v):
        self._d[self._idx(i)] = v
    def __delitem__(self, i):
        self._norm()
        del self._d[self._idx(i)]
    def append(self, x):
        self._d.append(x)
        if self.maxlen is not None and len(self._d) - self._h > self.maxlen:
            self.popleft()
    def appendleft(self, x):
        if self._h > 0:
            self._h -= 1
            self._d[self._h] = x
        else:
            self._d.insert(0, x)
        if self.maxlen is not None and len(self._d) - self._h > self.maxlen:
            self._d.pop()
    def pop(self):
        if len(self._d) - self._h == 0:
            raise IndexError("pop from an empty deque")
        return self._d.pop()
    def popleft(self):
        if len(self._d) - self._h == 0:
            raise IndexError("pop from an empty deque")
        x = self._d[self._h]
        self._h += 1
        if self._h > 32 and self._h * 2 > len(self._d):
            self._norm()
        return x
    def extend(self, it):
        for x in list(it):
            self.append(x)
    def extendleft(self, it):
        for x in list(it):
            self.appendleft(x)
    def clear(self):
        self._d = []
        self._h = 0
    def copy(self):
        return deque(self._d[self._h:], self.maxlen)
    def count(self, x):
        return self._d[self._h:].count(x)
    def index(self, x):
        return self._d[self._h:].index(x)
    def remove(self, x):
        self._norm()
        self._d.remove(x)
    def reverse(self):
        self._norm()
        self._d.reverse()
    def insert(self, i, x):
        self._norm()
        self._d.insert(i, x)
    def rotate(self, n=1):
        self._norm()
        k = len(self._d)
        if k:
            n = n % k
            self._d = self._d[k - n:] + self._d[:k - n]
    def __eq__(self, other):
        return isinstance(other, deque) and list(self) == list(other)
    def __repr__(self):
        items = list(self._d[self._h:])
        if self.maxlen is None:
            return "deque(" + repr(items) + ")"
        return "deque(" + repr(items) + ", maxlen=" + repr(self.maxlen) + ")"

def namedtuple(typename, field_names, defaults=None, rename=False):
    if isinstance(field_names, str):
        field_names = field_names.replace(",", " ").split()
    fields = tuple(field_names)
    nf = len(fields)
    defs = tuple(defaults) if defaults is not None else ()
    def __init__(self, *args, **kwargs):
        vals = list(args)
        if len(vals) > nf:
            raise TypeError(f"{typename}.__new__() takes {nf + 1} positional arguments but {len(vals) + 1} were given")
        for i in range(len(vals), nf):
            name = fields[i]
            if name in kwargs:
                vals.append(kwargs.pop(name))
            elif i - (nf - len(defs)) >= 0:
                vals.append(defs[i - (nf - len(defs))])
            else:
                raise TypeError(f"{typename}.__new__() missing 1 required positional argument: '{name}'")
        if kwargs:
            raise TypeError(f"{typename}.__new__() got an unexpected keyword argument '{list(kwargs)[0]}'")
        self._values = tuple(vals)
        for i in range(nf):
            setattr(self, fields[i], vals[i])
    def __iter__(self):
        return iter(self._values)
    def __len__(self):
        return nf
    def __getitem__(self, i):
        return self._values[i]
    def __eq__(self, other):
        return tuple(self._values) == tuple(other)
    def __lt__(self, other):
        return tuple(self._values) < tuple(other)
    def __hash__(self):
        return hash(self._values)
    def __repr__(self):
        return typename + "(" + ", ".join(f"{fields[i]}={self._values[i]!r}" for i in range(nf)) + ")"
    def _asdict(self):
        return dict(zip(fields, self._values))
    def _replace(self, **kw):
        d = self._asdict()
        d.update(kw)
        return ctor(**d)
    def count(self, x):
        return self._values.count(x)
    def index(self, x):
        return self._values.index(x)
    ns = {"_fields": fields, "__init__": __init__, "__iter__": __iter__, "__len__": __len__, "__getitem__": __getitem__,
          "__eq__": __eq__, "__lt__": __lt__, "__hash__": __hash__, "__repr__": __repr__, "_asdict": _asdict,
          "_replace": _replace, "count": count, "index": index, "_field_defaults": dict(zip(fields[nf - len(defs):], defs))}
    ctor = type(typename, (), ns)
    return ctor

class ChainMap:
    def __init__(self, *maps):
        self.maps = list(maps) if maps else [{}]
    def __getitem__(self, key):
        for m in self.maps:
            if key in m:
                return m[key]
        raise KeyError(key)
    def get(self, key, default=None):
        for m in self.maps:
            if key in m:
                return m[key]
        return default
    def __setitem__(self, key, value):
        self.maps[0][key] = value
    def __contains__(self, key):
        return any(key in m for m in self.maps)
    def __iter__(self):
        seen = []
        for m in reversed(self.maps):
            for k in m:
                if k not in seen:
                    seen.append(k)
        return iter(seen)
    def __len__(self):
        return len(list(iter(self)))
    def keys(self):
        return list(iter(self))
    def items(self):
        return [(k, self[k]) for k in self]
    def values(self):
        return [self[k] for k in self]
    def new_child(self, m=None):
        return ChainMap(m if m is not None else {}, *self.maps)
