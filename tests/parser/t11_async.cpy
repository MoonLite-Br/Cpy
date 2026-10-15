import asyncio
async def f():
    return 42
async def main():
    v = await f()
    return v
print(asyncio.run(main()))
