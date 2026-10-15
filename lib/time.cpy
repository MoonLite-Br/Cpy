from _time import time, time_ns, sleep, monotonic, monotonic_ns, perf_counter, perf_counter_ns, _localtime, _gmtime, _mktime, _strftime, _strptime, _tzoffset

timezone = _tzoffset()
altzone = timezone
daylight = 0
tzname = ("UTC", "UTC")
_FIELDS = ("tm_year", "tm_mon", "tm_mday", "tm_hour", "tm_min", "tm_sec", "tm_wday", "tm_yday", "tm_isdst")

class struct_time:
    def __init__(self, seq):
        seq = tuple(seq)
        self._t = seq
        for i in range(len(_FIELDS)):
            setattr(self, _FIELDS[i], seq[i] if i < len(seq) else 0)
    def __getitem__(self, i):
        return self._t[i]
    def __len__(self):
        return len(self._t)
    def __iter__(self):
        return iter(self._t)
    def __eq__(self, other):
        return tuple(self._t) == tuple(other)
    def __hash__(self):
        return hash(self._t)
    def __repr__(self):
        parts = ", ".join(f"{_FIELDS[i]}={self._t[i]}" for i in range(9))
        return "time.struct_time(" + parts + ")"

def localtime(secs=None):
    if secs is None:
        secs = time()
    return struct_time(_localtime(secs))

def gmtime(secs=None):
    if secs is None:
        secs = time()
    return struct_time(_gmtime(secs))

def mktime(t):
    return _mktime(tuple(t))

def strftime(fmt, t=None):
    if t is None:
        t = localtime()
    return _strftime(fmt, tuple(t))

def strptime(string, fmt="%a %b %d %H:%M:%S %Y"):
    return struct_time(_strptime(string, fmt))

def asctime(t=None):
    if t is None:
        t = localtime()
    return _strftime("%a %b %e %H:%M:%S %Y", tuple(t))

def ctime(secs=None):
    return asctime(localtime(secs))

def process_time():
    return perf_counter()

def thread_time():
    return perf_counter()
