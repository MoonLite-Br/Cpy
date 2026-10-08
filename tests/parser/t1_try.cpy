def f(n):
    try:
        return 10 // n
    except ZeroDivisionError as e:
        return -1
    finally:
        pass
print(f(2), f(0))
