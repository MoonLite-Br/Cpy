print("=== cpy demo ===")

def add(a, b):
    return a + b

print(add(10, 5))

def my_abs(x):
    if x < 0:
        return -x
    return x

print(my_abs(-7))

write("demo_out.txt", "hello from cpy")
print(read("demo_out.txt"))

for i in range(3):
    print(i)
