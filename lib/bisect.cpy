def bisect_left(a, x, lo=0, hi=None, key=None):
    if hi is None:
        hi = len(a)
    while lo < hi:
        mid = (lo + hi) // 2
        v = a[mid] if key is None else key(a[mid])
        if v < x:
            lo = mid + 1
        else:
            hi = mid
    return lo

def bisect_right(a, x, lo=0, hi=None, key=None):
    if hi is None:
        hi = len(a)
    while lo < hi:
        mid = (lo + hi) // 2
        v = a[mid] if key is None else key(a[mid])
        if x < v:
            hi = mid
        else:
            lo = mid + 1
    return lo

bisect = bisect_right

def insort_left(a, x, lo=0, hi=None, key=None):
    a.insert(bisect_left(a, x if key is None else key(x), lo, hi, key), x)

def insort_right(a, x, lo=0, hi=None, key=None):
    a.insert(bisect_right(a, x if key is None else key(x), lo, hi, key), x)

insort = insort_right
