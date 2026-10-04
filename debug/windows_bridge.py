"""Drive native Windows Edge from WSL over a local child-process pipe.

The dedicated Edge profile listens only on Windows loopback. This avoids
opening a network port forward or accidentally testing in an invisible WSL GUI.
"""
from __future__ import annotations
import asyncio
import json
import os
from pathlib import Path
from playwright.async_api import TimeoutError as PlaywrightTimeout

ROOT = Path(__file__).resolve().parents[1]


def windows_path(path):
    text = str(path)
    if not text.startswith('/mnt/c/'):
        raise ValueError('Native test files must be in the Windows workspace')
    return 'C:/' + text[len('/mnt/c/'):]


class Bridge:
    async def start(self):
        core = ROOT / 'out/windows-driver/node_modules/playwright-core'
        if not (core / 'package.json').exists():
            raise RuntimeError('Run npm install --prefix out/windows-driver --no-save --ignore-scripts playwright-core')
        self.pending: dict[int, asyncio.Future] = {}
        self.counter = 0
        self.page: NativePage | None = None
        self.process = await asyncio.create_subprocess_exec(
            '/mnt/c/Program Files/nodejs/node.exe',
            windows_path(ROOT / 'debug/native_driver.cjs'), windows_path(core),
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            limit=8 * 1024 * 1024,
            env={**os.environ, 'WSL_INTEROP': '/run/WSL/1_interop'})
        self.reader = asyncio.create_task(self.read())
        return self

    async def read(self):
        assert self.process.stdout is not None and self.process.stderr is not None
        while line := await self.process.stdout.readline():
            message = json.loads(line)
            if 'event' in message:
                if self.page:
                    if message['event'] == 'close':
                        self.page.closed = True
                    for callback in self.page.handlers.get(message['event'], []):
                        callback(message.get('text', ''))
                continue
            future = self.pending.pop(message['id'], None)
            if future and not future.done():
                if 'error' in message:
                    error = message['error']
                    future.set_exception(PlaywrightTimeout(error) if 'TimeoutError' in error else RuntimeError(error))
                else:
                    future.set_result(message.get('result'))
        error = (await self.process.stderr.read()).decode(errors='replace')
        for future in self.pending.values():
            if not future.done():
                future.set_exception(RuntimeError('Windows browser bridge closed: ' + error))

    async def call(self, operation, **args):
        self.counter += 1
        future = asyncio.get_running_loop().create_future()
        self.pending[self.counter] = future
        assert self.process.stdin is not None
        self.process.stdin.write((json.dumps({'id': self.counter, 'method': operation, 'args': args}) + '\n').encode())
        await self.process.stdin.drain()
        return await future


class NativeSession:
    def __init__(self, bridge): self.bridge = bridge
    async def send(self, method, params=None):
        return await self.bridge.call('cdp', method=method, params=params or {})


class NativeContext:
    def __init__(self, bridge): self.bridge = bridge
    async def new_cdp_session(self, page): return NativeSession(self.bridge)


class NativePage:
    def __init__(self, bridge):
        self.bridge, self.closed = bridge, False
        self.handlers: dict[str, list] = {}
        self.context = NativeContext(bridge)
    def on(self, name, callback): self.handlers.setdefault(name, []).append(callback)
    def is_closed(self): return self.closed
    async def goto(self, url): return await self.bridge.call('goto', url=url)
    async def evaluate(self, expression, argument=None):
        return await self.bridge.call('evaluate', expression=expression, argument=argument)
    async def wait_for_function(self, expression, timeout=90000):
        return await self.bridge.call('wait', expression=expression, timeout=timeout)
    async def bring_to_front(self): return await self.bridge.call('front')
    async def wait_for_timeout(self, milliseconds): await asyncio.sleep(milliseconds / 1000)
    async def wait_for_event(self, event, timeout):
        if event != 'close': raise ValueError('Unsupported native bridge event')
        return await self.bridge.call('waitClosed', timeout=timeout)


class WindowsBrowser:
    bridge: Bridge
    @classmethod
    async def connect(cls):
        instance = cls()
        instance.bridge = await Bridge().start()
        return instance
    async def new_page(self, viewport):
        await self.bridge.call('init', viewport=viewport)
        page = NativePage(self.bridge)
        self.bridge.page = page
        return page
    async def close(self):
        # Disconnect automation; do not close the user's visible Windows app.
        if self.bridge.process.returncode is None:
            await self.bridge.call('disconnect')
            await self.bridge.process.wait()
