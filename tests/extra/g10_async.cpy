async def fetch(n):
    return n * 2

async def main():
    a = await fetch(3)
    b = await fetch(4)
    return a + b

import asyncio
print(asyncio.run(main()))
