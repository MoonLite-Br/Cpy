# helper module used by t08_modules.cpy
counter = 0
PI2 = 6.28

def bump(n=1):
    global counter
    counter += n
    return counter

class Greeter:
    def __init__(self, who):
        self.who = who
    def greet(self):
        return "hello " + self.who

if __name__ == "__main__":
    print("helper run directly")
else:
    print("helper imported as", __name__)
