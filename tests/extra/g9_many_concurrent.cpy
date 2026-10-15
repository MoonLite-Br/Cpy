def gen(n, start):
    for i in range(n):
        yield start + i

gens = [gen(3, i * 10) for i in range(20)]
results = []
for g in gens:
    results.append(list(g))
print(results)
