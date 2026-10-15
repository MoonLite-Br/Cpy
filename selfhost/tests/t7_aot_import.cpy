import math

def dist(x1, y1, x2, y2):
    dx = x1 - x2
    dy = y1 - y2
    return math.sqrt(dx * dx + dy * dy)

print(dist(0, 0, 3, 4))
print(math.pi > 3.14)
print(math.floor(3.7), math.ceil(3.2))
