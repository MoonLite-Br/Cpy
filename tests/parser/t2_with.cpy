class Ctx:
    def __enter__(self):
        return 5
    def __exit__(self, a, b, c):
        return False
with Ctx() as v:
    print(v)
