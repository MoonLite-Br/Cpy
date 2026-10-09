import math
import helper_mod
import helper_mod as hm
from helper_mod import bump, Greeter as G, PI2
from math import sqrt, pi
import random
import time
import sys
import os

print(math.sqrt(16), math.floor(2.7), math.ceil(2.1), math.floor(-2.5), math.pi, math.e, sqrt(2), round(pi, 5))
print(math.gcd(12, 18), math.factorial(10), math.isqrt(99), math.log(math.e), math.log2(8), math.log10(1000), math.hypot(3, 4))
print(math.sin(0), math.cos(0), math.pow(2, 0.5), math.fabs(-3), math.trunc(-2.7), math.isnan(math.nan), math.isinf(math.inf), math.degrees(math.pi))
print(helper_mod.bump(), bump(5), hm.counter, helper_mod is hm, G("bob").greet(), PI2)
random.seed(7)
r = random.random()
print(0 <= r < 1, 1 <= random.randint(1, 6) <= 6, random.choice([5]), random.uniform(1, 2) >= 1)
lst = list(range(10))
random.shuffle(lst)
print(sorted(lst))
t0 = time.time()
time.sleep(0.01)
print(time.time() - t0 >= 0.005, time.perf_counter() > 0)
print(type(sys.argv), len(sys.argv) >= 1, os.getenv("SURELY_NOT_SET_XYZ"), os.getenv("SURELY_NOT_SET_XYZ", "dflt"))
sys.stdout.write("via stdout\n")

with open("tmp_cpy_test.txt", "w") as f:
    f.write("line one\n")
    f.write("line two\n")
    n = f.write("three")
print(n)
with open("tmp_cpy_test.txt") as f:
    print(f.read())
with open("tmp_cpy_test.txt", "r") as f:
    print(f.readline(), end="|")
    print(f.readline().strip())
    print(f.readlines())
with open("tmp_cpy_test.txt") as f:
    for i, line in enumerate(f):
        print(i, line.rstrip("\n"))
with open("tmp_cpy_test.txt", "a") as f:
    f.write("\nappended")
f = open("tmp_cpy_test.txt")
data = f.read()
f.close()
print(len(data), data.count("\n"))
try:
    f.read()
except ValueError as e:
    print("closed:", e)
os.remove("tmp_cpy_test.txt")
try:
    open("tmp_cpy_test.txt")
except FileNotFoundError:
    print("gone")
try:
    import no_such_module_xyz
except ImportError as e:
    print("ImportError:", e)
class Ctx:
    def __enter__(self):
        print("enter")
        return self
    def __exit__(self, a, b, c):
        print("exit", a is not None)
        return False
with Ctx() as c:
    print("inside")
try:
    with Ctx():
        raise ValueError("boom")
except ValueError:
    print("propagated")
