class Vec:
    def __init__(self, x, y):
        self.x = x
        self.y = y
    def add(self, o):
        return Vec(self.x + o.x, self.y + o.y)
    def dot(self, o):
        return self.x * o.x + self.y * o.y
def run(n):
    a = Vec(1, 2)
    acc = Vec(0, 0)
    t = 0
    for i in range(n):
        acc = acc.add(a)
        t += acc.dot(a) % 7
    return acc.x, t
print(run(300000))
