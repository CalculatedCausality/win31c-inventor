"""Optional real-browser smoke test; requires locally installed Python Playwright.

Start server first. Run: python3 debug/smoke.py
"""
import asyncio
import json
from pathlib import Path
from playwright.async_api import async_playwright


async def main():
    async with async_playwright() as p:
        browser = await p.chromium.launch(headless=True)
        page = await browser.new_page(viewport={'width': 1100, 'height': 1000})
        errors, requests = [], []
        page.on('pageerror', lambda e: errors.append(str(e)))
        page.on('request', lambda r: requests.append(r.url))
        page.on('response', lambda r: errors.append(f'HTTP {r.status}: {r.url}') if r.status >= 400 else None)
        page.on('dialog', lambda d: d.accept())
        await page.goto('http://127.0.0.1:8310/', wait_until='domcontentloaded')
        await page.wait_for_function('window.cadDebug?.status().ready', timeout=180000)
        assert await page.locator('#cad canvas').count() == 1
        assert await page.locator('iframe').count() == 0
        await page.evaluate('cadDebug.pause()')
        assert not await page.evaluate('cadDebug.status().running')
        await page.evaluate('cadDebug.run()')
        assert await page.evaluate('cadDebug.status().running')
        await page.evaluate('cadDebug.speed()')
        mounted = await page.evaluate('cadDebug.mountLatest()')
        assert mounted['drive'] == 'A:' and mounted['bytes'] > 0
        server = await page.evaluate('fetch("/api/status").then(r => r.json())')
        assert mounted['sha256'] == server['disk']['sha256']
        assert 'ASCII' in await page.evaluate('cadDebug.type("").catch(e => e.message)')
        await page.wait_for_timeout(30000)
        await page.evaluate('cadDebug.launchHouse()')
        await page.wait_for_timeout(6000)
        await page.screenshot(path=str(Path(__file__).with_name('smoke.png')))
        async with page.expect_download() as download:
            await page.locator('[data-action=screenshot]').click()
        downloaded = await download.value
        assert downloaded.suggested_filename.endswith('.png')
        await downloaded.save_as(str(Path(__file__).with_name('guest.png')))
        await page.evaluate('cadDebug.reset()')
        await page.wait_for_timeout(500)
        assert await page.evaluate('cadDebug.status().running')
        assert all(url.startswith('http://127.0.0.1:8310/') for url in requests), requests
        assert not errors, errors
        print(json.dumps(await page.evaluate('cadDebug.status()'), indent=2))
        print('PASS: isolated local machine, pause/run/speed/reset, native mount + disk hash, keyboard HOUSE sequence, PNG download, no failed resources')
        await browser.close()


if __name__ == '__main__':
    asyncio.run(main())
