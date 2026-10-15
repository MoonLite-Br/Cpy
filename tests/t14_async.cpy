import asyncio

async def double(x):
    return x * 2

async def main_basic():
    a = await double(5)
    b = await double(a)
    return a + b

print(asyncio.run(main_basic()))

async def helper(x):
    await asyncio.sleep(0.001)
    return x + 1

async def chain(n):
    v = 0
    for _ in range(n):
        v = await helper(v)
    return v

print(asyncio.run(chain(4)))

async def worker(tag, delay):
    await asyncio.sleep(delay)
    return tag

async def gather_test():
    return await asyncio.gather(worker("a", 0.001), worker("b", 0.001), worker("c", 0.001))

print(asyncio.run(gather_test()))

async def boom():
    await asyncio.sleep(0.001)
    raise ValueError("kaboom")

async def catch_gather():
    try:
        await asyncio.gather(worker("ok", 0.001), boom())
        return "no error"
    except ValueError as e:
        return "caught: " + str(e)

print(asyncio.run(catch_gather()))

async def bg_main():
    t = asyncio.create_task(worker("bg", 0.002))
    fg = await worker("fg", 0.001)
    bg = await t
    return (fg, bg)

print(asyncio.run(bg_main()))

async def uses_cancel():
    t = asyncio.create_task(worker("x", 1))
    ok = t.cancel()
    return (ok, t.done, t.cancelled)

print(asyncio.run(uses_cancel()))

async def bad_await():
    return await 5

async def catch_bad():
    try:
        await bad_await()
        return "no error"
    except TypeError as e:
        return "TypeError"

print(asyncio.run(catch_bad()))

coro = double(3)
print(type(coro).__name__)
coro.close()

print("done")
