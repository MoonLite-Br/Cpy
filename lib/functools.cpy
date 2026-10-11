def reduce(function, iterable, *initial):
    it = iter(iterable)
    if initial:
        value = initial[0]
    else:
        try:
            value = next(it)
        except StopIteration:
            raise TypeError("reduce() of empty iterable with no initial value")
    for x in it:
        value = function(value, x)
    return value

class partial:
    def __init__(self, func, *args, **kwargs):
        self.func = func
        self.args = args
        self.keywords = kwargs
    def __call__(self, *args, **kwargs):
        kw = dict(self.keywords)
        kw.update(kwargs)
        return self.func(*self.args, *args, **kw)

def wraps(wrapped, assigned=None, updated=None):
    def deco(f):
        try:
            f.__name__ = wrapped.__name__
            f.__doc__ = wrapped.__doc__
        except AttributeError:
            pass
        return f
    return deco

def update_wrapper(wrapper, wrapped, assigned=None, updated=None):
    return wraps(wrapped)(wrapper)

def _make_cache(f, maxsize):
    cache = {}
    stats = [0, 0]
    def wrapper(*args, **kwargs):
        key = args if not kwargs else (args, tuple(sorted(kwargs.items())))
        if key in cache:
            stats[0] += 1
            return cache[key]
        stats[1] += 1
        v = f(*args, **kwargs)
        cache[key] = v
        if maxsize is not None and len(cache) > maxsize:
            del cache[next(iter(cache))]
        return v
    def cache_clear():
        cache.clear()
        stats[0] = 0
        stats[1] = 0
    def cache_info():
        return _CacheInfo(stats[0], stats[1], maxsize, len(cache))
    wrapper.cache_clear = cache_clear
    wrapper.cache_info = cache_info
    wrapper.__name__ = f.__name__
    return wrapper

class _CacheInfo:
    def __init__(self, hits, misses, maxsize, currsize):
        self.hits = hits
        self.misses = misses
        self.maxsize = maxsize
        self.currsize = currsize
    def __repr__(self):
        return f"CacheInfo(hits={self.hits}, misses={self.misses}, maxsize={self.maxsize}, currsize={self.currsize})"

def lru_cache(maxsize=128, typed=False):
    if callable(maxsize):
        return _make_cache(maxsize, 128)
    def deco(f):
        return _make_cache(f, maxsize)
    return deco

def cache(f):
    return _make_cache(f, None)

class cmp_to_key:
    def __init__(self, mycmp):
        self.mycmp = mycmp
        self.obj = None
    def __call__(self, obj):
        k = cmp_to_key(self.mycmp)
        k.obj = obj
        return k
    def __lt__(self, other):
        return self.mycmp(self.obj, other.obj) < 0
    def __gt__(self, other):
        return self.mycmp(self.obj, other.obj) > 0
    def __eq__(self, other):
        return self.mycmp(self.obj, other.obj) == 0

def total_ordering(cls):
    d = cls.__dict__
    if "__lt__" in d:
        if "__gt__" not in d:
            cls.__gt__ = lambda a, b: b < a
        if "__le__" not in d:
            cls.__le__ = lambda a, b: not (b < a)
        if "__ge__" not in d:
            cls.__ge__ = lambda a, b: not (a < b)
    elif "__gt__" in d:
        if "__lt__" not in d:
            cls.__lt__ = lambda a, b: b > a
        if "__ge__" not in d:
            cls.__ge__ = lambda a, b: not (b > a)
        if "__le__" not in d:
            cls.__le__ = lambda a, b: not (a > b)
    return cls
