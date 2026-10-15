# lib/asyncio.cpy - a real, single-threaded, cooperative event loop, in the
# spirit of Python's own asyncio (concurrency via taking turns at `await`
# points, not OS threads or real parallelism -- exactly how CPython's
# asyncio itself works internally).
#
# How `await` gets here: `await expr` at the interpreter level either
# delegates transparently into another cpy coroutine (like `yield from`,
# when expr is itself produced by an `async def`), or -- when expr is some
# other value -- suspends the current coroutine and hands expr *up*,
# unchanged, to whoever is driving it. That's this module's event loop: it
# interprets whatever bubbles up (a sleep marker, a Task to wait on) and
# decides when to resume the coroutine that awaited it.
import time
import heapq

_SLEEP = "\x00cpy.asyncio.sleep\x00"   # unlikely to collide with real user data


class CancelledError(Exception):
    pass


class InvalidStateError(Exception):
    pass


async def sleep(seconds, result=None):
    """Suspend the current task for `seconds` without blocking other tasks."""
    await (_SLEEP, seconds)
    return result


class Task:
    """A coroutine scheduled to run on the event loop. Awaiting a Task waits
    for it to finish and yields its result (or re-raises its exception)."""

    def __init__(self, coro, loop, name=None):
        self._coro = coro
        self._loop = loop
        self.name = name
        self.done = False
        self.cancelled = False
        self.result = None
        self.exception = None
        self._callbacks = []

    def add_done_callback(self, cb):
        if self.done:
            cb(self)
        else:
            self._callbacks.append(cb)

    def _finish(self, result=None, exc=None):
        self.done = True
        self.result = result
        self.exception = exc
        for cb in self._callbacks:
            cb(self)
        self._callbacks = []

    def cancel(self):
        if self.done:
            return False
        self.cancelled = True
        self._finish(exc=CancelledError())
        return True


class Loop:
    def __init__(self):
        self._ready = []       # [(task, send_value), ...] runnable this tick
        self._sleeping = []    # heap of (wake_time, seq, task, send_value)
        self._seq = 0

    def _schedule(self, task, value=None):
        self._ready.append((task, value))

    def _schedule_after(self, delay, task, value=None):
        self._seq += 1
        heapq.heappush(self._sleeping, (time.time() + delay, self._seq, task, value))

    def create_task(self, coro, name=None):
        t = Task(coro, self, name=name)
        self._schedule(t, None)
        return t

    def _resume(self, task, value=None, exc=None):
        """Resume `task` by sending `value` into it, or -- if a sub-task it
        was waiting on failed -- by throwing `exc` in at the await point."""
        if task.cancelled:
            return
        try:
            yielded = task._coro.throw(exc) if exc is not None else task._coro.send(value)
        except StopIteration as e:
            task._finish(result=getattr(e, "value", None))
            return
        except Exception as e:
            task._finish(exc=e)
            return
        if isinstance(yielded, tuple) and len(yielded) == 2 and yielded[0] == _SLEEP:
            self._schedule_after(yielded[1], task, None)
            return
        if isinstance(yielded, Task):
            def _wake(inner, task=task):
                self._schedule(task, inner)
            yielded.add_done_callback(_wake)
            return
        # unrecognized awaitable: this means some `await x` inside the task
        # handed the event loop something that isn't a coroutine, a Task, or
        # our sleep marker. Fail the task with a clear error instead of
        # resuming it with None and masking the mistake.
        try:
            task._coro.throw(TypeError(
                f"object {type(yielded).__name__} can't be used in 'await' expression"))
        except StopIteration as e:
            task._finish(result=getattr(e, "value", None))
        except Exception as e:
            task._finish(exc=e)

    def run_until_complete(self, task):
        while not task.done:
            ran_any = False
            while self._ready:
                t, value = self._ready.pop(0)
                if t.done:
                    continue
                if isinstance(value, Task):
                    # a completed sub-task woke us up: hand back its result/exception
                    inner = value
                    if inner.exception is not None:
                        self._resume(t, exc=inner.exception)
                    else:
                        self._resume(t, value=inner.result)
                else:
                    self._resume(t, value=value)
                ran_any = True
            if task.done:
                break
            if self._sleeping:
                now = time.time()
                wake_at = self._sleeping[0][0]
                if wake_at > now:
                    time.sleep(wake_at - now)
                while self._sleeping and self._sleeping[0][0] <= time.time():
                    _, _, t, value = heapq.heappop(self._sleeping)
                    if not t.done:
                        self._schedule(t, value)
            elif not ran_any and not self._ready:
                break
        if task.exception is not None:
            raise task.exception
        return task.result


_default_loop = Loop()


def get_event_loop():
    return _default_loop


def new_event_loop():
    return Loop()


def create_task(coro):
    return _default_loop.create_task(coro)


def run(coro):
    """Run a top-level coroutine to completion (like Python's asyncio.run)."""
    t = _default_loop.create_task(coro)
    return _default_loop.run_until_complete(t)


async def gather(*aws, return_exceptions=False):
    tasks = [a if isinstance(a, Task) else create_task(a) for a in aws]
    results = []
    for t in tasks:
        got = await t
        results.append(got)
    return results


async def wait_for(aw, timeout):
    task = aw if isinstance(aw, Task) else create_task(aw)
    start = time.time()
    while not task.done:
        if time.time() - start > timeout:
            task.cancel()
            raise TimeoutError("timed out waiting for task")
        await sleep(0.01)
    if task.exception is not None:
        raise task.exception
    return task.result
