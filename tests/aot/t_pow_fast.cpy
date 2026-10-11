def sq(x):
    return x ** 2

def cube(x):
    return x ** 3

def fpow(x, y):
    return x ** y  # float base -- always float, any exponent

def ipow_var_exp(x, y):
    return x ** y  # int ** int, exponent NOT a constant -- must stay boxed

def augpow(x):
    x **= 2
    return x

def augpow_neg(x):
    y = x
    y **= -1  # int **= negative const -- NOT representable as int, must poison
    return y

print(sq(5), cube(3), fpow(2.0, 10), fpow(2.0, -1), ipow_var_exp(2, 10), augpow(4))
print(ipow_var_exp(5, -2))
print(augpow_neg(4))
