# return type differs across branches -- must NOT be unboxed (stays boxed;
# real python keeps int 1 distinct from float 1.0 here)
def classify(x):
    if x > 0:
        return 1
    else:
        return 1.0

print(classify(5))
print(classify(-5))
