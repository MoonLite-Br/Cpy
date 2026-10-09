def flatten(x):
    if isinstance(x, list):
        for item in x:
            for v in flatten(item):
                yield v
    else:
        yield x

print(list(flatten([1, [2, 3, [4, 5]], 6, [[7]]])))
