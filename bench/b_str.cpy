def build(n):
    parts = []
    s = ""
    for i in range(n):
        s += "ab"
        parts.append(str(i))
    d = {}
    for i in range(n):
        d[i % 1000] = d.get(i % 1000, 0) + i
    return len(s), len(",".join(parts)), len(d), sum(d.values())
print(build(300000))
