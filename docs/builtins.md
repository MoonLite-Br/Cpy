# Builtins, methods and modules

## Functions

`print input len int float str bool repr list tuple dict set frozenset range type isinstance issubclass`
`abs min max sum round divmod pow chr ord hex bin oct format sorted reversed enumerate zip map filter any all`
`hasattr getattr setattr callable id hash iter next open read write globals vars help exit quit super`
`staticmethod classmethod property type(name, bases, dict)`

`sorted/reversed/enumerate/zip/map/filter/iter` return **lazy iterators** (like real Python 3), not lists — wrap in `list(...)` if you need one.

## str methods

`upper lower capitalize title swapcase strip lstrip rstrip split(sep=None, maxsplit=-1) join replace(old, new[, count]) find rfind index rindex count startswith endswith isdigit isalpha isalnum isspace isupper islower ljust rjust center zfill splitlines partition format`

`"{} {name:>6.2f}".format(1, name=3.14159)` — `{}`, `{0}`, `{name}`, `!r`, format specs.
`"%d %s %5.2f %x %r %%" % (…)` — `d i s r f e g x X o c %` with flags, width, precision.

## list / tuple / dict / set methods

- **list**: `append extend insert pop([i]) remove index count sort(key=None, reverse=False) reverse copy clear`
- **tuple**: `index count`
- **dict**: `keys values items get pop update setdefault copy clear popitem move_to_end fromkeys` — `d | d2` merges, `d1 |= d2` updates in place
- **set / frozenset**: `add remove discard pop clear copy update union intersection difference symmetric_difference issubset issuperset isdisjoint intersection_update difference_update` — plus `| & - ^` and their `|=` etc.

## Generators & iterators

`gen.send(v) gen.close() gen.throw(exc)`, and every iterator supports `next(it[, default])` / `for x in it`.

## Modules

**math** — `sqrt sin cos tan asin acos atan atan2 sinh cosh tanh exp log(x[, base]) log2 log10 pow fabs floor ceil trunc hypot fmod isnan isinf radians degrees gcd factorial isqrt` and `pi e tau inf nan`.

**random** — `random seed randint randrange uniform choice choices sample shuffle gauss/normalvariate getrandbits`

**time** — `time time_ns sleep perf_counter monotonic localtime gmtime mktime strftime strptime asctime ctime process_time`, `struct_time`

**sys** — `argv exit path modules stdin stdout stderr version version_info platform maxsize getrecursionlimit setrecursionlimit`

**os** — `getenv environ getcwd chdir listdir mkdir makedirs rmdir remove rename system getpid cpu_count`; **os.path** — `join exists isfile isdir basename dirname split splitext abspath realpath normpath expanduser isabs getsize getmtime`

**collections** — `deque(maxlen=) Counter(most_common, elements, total, subtract) defaultdict OrderedDict namedtuple ChainMap`

**itertools** — `count cycle repeat chain(.from_iterable) islice product permutations combinations combinations_with_replacement groupby accumulate zip_longest takewhile dropwhile starmap compress pairwise filterfalse tee batched`

**functools** — `reduce partial wraps update_wrapper lru_cache cache cmp_to_key total_ordering`

**operator** — arithmetic/comparison functions, `itemgetter attrgetter methodcaller`

**heapq** — `heappush heappop heapify heapreplace heappushpop nlargest nsmallest merge`

**bisect** — `bisect_left bisect_right/bisect insort_left insort_right/insort`

**string** — `ascii_letters digits punctuation whitespace ... capwords Template`

Your own `.cpy`/`.py` files and packages import the same way (`sys.path`, `$CPY_PATH`).
