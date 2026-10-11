class Animal:
    def __init__(self, name):
        self.name = name
    def speak(self):
        return "..."
    def intro(self):
        return self.name + " says " + self.speak()

class Dog(Animal):
    def speak(self):
        return "Woof"

a = Animal("Generic")
d = Dog("Rex")
print(a.intro())
print(d.intro())

class Counter:
    def __init__(self):
        self.n = 0
    def inc(self):
        self.n = self.n + 1
        return self.n

c = Counter()
print(c.inc(), c.inc(), c.inc())
