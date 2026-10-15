def mandel(w, h, it):
    count = 0
    for y in range(h):
        for x in range(w):
            cr = 2.0 * x / w - 1.5
            ci = 2.0 * y / h - 1.0
            zr = 0.0
            zi = 0.0
            k = 0
            while k < it and zr * zr + zi * zi < 4.0:
                t = zr * zr - zi * zi + cr
                zi = 2.0 * zr * zi + ci
                zr = t
                k += 1
            if k == it:
                count += 1
    return count
print(mandel(120, 80, 60))
