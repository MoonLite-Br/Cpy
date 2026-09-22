from _itertools import count, cycle, repeat, islice, takewhile, dropwhile, starmap, filterfalse, zip_longest, accumulate, compress, pairwise
from _itertools import chain as _chain, chain_from_iterable as _chain_from

class chain:
    def __init__(self, *iterables):
        self._it = _chain(*iterables)
    def __iter__(self):
        return self._it
    def __next__(self):
        return next(self._it)
    @staticmethod
    def from_iterable(iterables):
        c = chain()
        c._it = _chain_from(iterables)
        return c

def product(*iterables, repeat=1):
    pools = [tuple(p) for p in iterables] * repeat
    result = [[]]
    for pool in pools:
        result = [x + [y] for x in result for y in pool]
    return iter([tuple(r) for r in result])

def permutations(iterable, r=None):
    pool = tuple(iterable)
    n = len(pool)
    r = n if r is None else r
    out = []
    if r > n:
        return iter(out)
    def rec(prefix, used):
        if len(prefix) == r:
            out.append(tuple(prefix))
            return
        for i in range(n):
            if i not in used:
                rec(prefix + [pool[i]], used | {i})
    rec([], set())
    return iter(out)

def combinations(iterable, r):
    pool = tuple(iterable)
    n = len(pool)
    out = []
    def rec(start, prefix):
        if len(prefix) == r:
            out.append(tuple(prefix))
            return
        for i in range(start, n):
            rec(i + 1, prefix + [pool[i]])
    rec(0, [])
    return iter(out)

def combinations_with_replacement(iterable, r):
    pool = tuple(iterable)
    n = len(pool)
    out = []
    def rec(start, prefix):
        if len(prefix) == r:
            out.append(tuple(prefix))
            return
        for i in range(start, n):
            rec(i, prefix + [pool[i]])
    rec(0, [])
    return iter(out)

def groupby(iterable, key=None):
    out = []
    cur = None
    group = None
    for x in iterable:
        k = key(x) if key is not None else x
        if group is None or k != cur:
            group = []
            cur = k
            out.append((k, group))
        group.append(x)
    return iter([(k, iter(g)) for k, g in out])

def tee(iterable, n=2):
    items = list(iterable)
    return tuple(iter(list(items)) for _ in range(n))

def batched(iterable, n):
    items = list(iterable)
    return iter([tuple(items[i:i + n]) for i in range(0, len(items), n)])
