"""Automated guest UI assertions, not merely screenshot capture.

Requires Python Playwright, Pillow and the tesseract executable. Start the
workspace server and build HOUSE first. Failures keep PNG/text diagnostics in
out/ui-tests. Each test uses a fresh guest; the user's app window is untouched.
"""
import asyncio
import base64
import io
import os
import re
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import Image
from playwright.async_api import async_playwright

OUTPUT = Path(__file__).resolve().parents[1] / 'out' / 'ui-tests'


def recognize(image, title=False, line=False):
    if title:
        # Read the real guest caption, excluding browser chrome. White glyphs
        # on the blue Win3.1 title bar become black on white for OCR.
        image = image.convert('RGB')
        pixels = image.load()
        rows = []
        for y in range(image.height):
            blue = [x for x in range(image.width)
                    if pixels[x, y][2] > 100 and pixels[x, y][0] < 70 and pixels[x, y][1] < 70]
            rows.append((len(blue), blue[0] if blue else 0, blue[-1] + 1 if blue else 0))
        bands = []
        y = 0
        while y < image.height:
            if rows[y][0] < image.width / 4:
                y += 1
                continue
            top = y
            while y + 1 < image.height and rows[y + 1][0] >= image.width / 4:
                y += 1
            bottom = y
            best_y = max(range(top, bottom + 1), key=lambda row: rows[row][0])
            count, left, right = rows[best_y]
            # Native dialogs have a wide blue border along the bottom. Rank
            # caption-height bands, not just the widest individual blue row.
            if bottom - top + 1 >= 7:
                bands.append((count * min(25, bottom - top + 1), left, top, right, bottom))
            y += 1
        if not bands:
            return ''
        _, left, top, right, bottom = max(bands)
        image = image.crop((left, top, right, bottom + 1))
        mask = Image.new('L', image.size)
        data = image.tobytes()
        mask.putdata([0 if min(data[i:i + 3]) > 210 else 255
                      for i in range(0, len(data), 3)])
        image = mask
    else:
        image = image.convert('L')
    image = image.resize((image.width * 2, image.height * 2), Image.Resampling.NEAREST)
    with tempfile.TemporaryDirectory() as scratch:
        path = Path(scratch) / 'frame.png'
        image.save(path)
        result = subprocess.run(['tesseract', str(path), 'stdout', '--psm', '7' if title or line else '11'],
                                capture_output=True, text=True, timeout=20, check=True,
                                env={**os.environ, 'OMP_THREAD_LIMIT': '1'})
    return re.sub(r'\s+', ' ', result.stdout).strip().lower()


def red_geometry(image):
    # VGA roof edges are red. Caption, cursor and time-independent text are
    # excluded by color; unchanged desktop icons cancel in comparisons.
    data = image.convert('RGB').tobytes()
    return frozenset(i // 3 for i in range(0, len(data), 3)
                     if data[i] > 200 and data[i + 1] < 60 and data[i + 2] < 60)


def white_fraction(image):
    data = image.convert('RGB').tobytes()
    return sum(min(data[i:i + 3]) > 230 for i in range(0, len(data), 3)) / (image.width * image.height)


async def frame(page):
    data = await page.evaluate("document.querySelector('#cad canvas').toDataURL('image/png')")
    return Image.open(io.BytesIO(base64.b64decode(data.split(',', 1)[1]))).convert('RGB')


async def wait_screen(page, name, condition, *, title=False, timeout=45, pixels=False, reader=None):
    deadline = time.monotonic() + timeout
    last_text = ''
    image = await frame(page)
    previous_pixels = None
    stable_frames = 0
    while time.monotonic() < deadline:
        image = await frame(page)
        if pixels:
            passed = condition(image)
            current_pixels = red_geometry(image)
            stable_frames = stable_frames + 1 if passed and current_pixels == previous_pixels else 0
            previous_pixels = current_pixels if passed else None
            passed = passed and stable_frames >= 2
        else:
            last_text = await asyncio.to_thread(reader, image) if reader else await asyncio.to_thread(recognize, image, title)
            passed = condition(last_text)
        if passed:
            image.save(OUTPUT / f'{name}.png')
            (OUTPUT / f'{name}.txt').write_text(last_text)
            print(f'PASS: {name}', flush=True)
            return image
        await page.wait_for_timeout(700)
    image.save(OUTPUT / f'{name}-FAILED.png')
    (OUTPUT / f'{name}-FAILED.txt').write_text(last_text)
    raise AssertionError(f'{name}: guest UI condition not met; OCR={last_text!r}')


async def keys(page, tokens):
    await page.evaluate('(tokens) => cadDebug.keys(tokens)', tokens)


async def dialog(page, name):
    return await wait_screen(page, name,
                             lambda text: 'file name' in text and 'directories' in text)


async def main():
    if not shutil.which('tesseract'):
        raise SystemExit('Install tesseract-ocr to run guest UI assertions.')
    OUTPUT.mkdir(parents=True, exist_ok=True)
    async with async_playwright() as p:
        browser = await p.chromium.launch(headless=True)
        page = await browser.new_page(viewport={'width': 1100, 'height': 1000})
        errors = []
        page.on('pageerror', lambda e: errors.append(str(e)))
        try:
            await page.goto('http://localhost:8310/')
            await page.wait_for_function('window.cadDebug?.status().ready', timeout=90000)
            await page.evaluate('cadDebug.speed()')
            await page.evaluate('cadDebug.speed()')
            await page.evaluate('cadDebug.mountLatest()')
            await wait_screen(page, 'windows-boot',
                              lambda text: 'program manager' in text, title=True, timeout=90)
            await page.evaluate('cadDebug.launchHouse()')
            await wait_screen(page, 'house-caption', lambda text: 'house' in text,
                              title=True)
            initial = await wait_screen(page, 'house-launched',
                lambda image: len(red_geometry(image)) > 200, pixels=True)
            original_roof = red_geometry(initial)
            await keys(page, '$altv$10h$10')
            await wait_screen(page, 'drawing-view',
                lambda image: len(red_geometry(image)) < len(original_roof) * .25 and
                              white_fraction(image) > .35, pixels=True)
            drawing_text = await asyncio.to_thread(recognize, await frame(page))
            assert 'front' in drawing_text and 'right' in drawing_text, drawing_text
            await keys(page, '$esc$10$altf$10d$10')
            await dialog(page, 'export-dialog')
            await keys(page, '$enter$10')
            await wait_screen(page, 'export-success',
                              lambda text: 'wireframe exported' in text)
            await keys(page, '$enter$10$altt$10m$10')
            await dialog(page, 'macro-dialog')
            await keys(page, '$enter$10')
            changed = await wait_screen(page, 'macro-changed-geometry',
                lambda image: len(red_geometry(image) - original_roof) > 100 and
                              len(red_geometry(image)) > len(original_roof) * .8,
                pixels=True)
            changed_roof = red_geometry(changed)
            await keys(page, '$ctrlz$10')
            await wait_screen(page, 'undo-restores-exact-roof',
                lambda image: red_geometry(image) == original_roof, pixels=True)
            await keys(page, '$ctrly$10')
            await wait_screen(page, 'redo-restores-exact-roof',
                lambda image: red_geometry(image) == changed_roof, pixels=True)
            await keys(page, '$altf$10s$10')
            await dialog(page, 'save-dialog')
            await keys(page, '$enter$10')
            await wait_screen(page, 'save-completed',
                lambda text: 'house.h31' in text, title=True)
            await keys(page, '$alte$10r$10')
            await wait_screen(page, 'reset-restores-original',
                lambda image: red_geometry(image) == original_roof, pixels=True)
            await keys(page, '$altf$10o$10')
            await dialog(page, 'open-dialog')
            await page.evaluate('(text) => cadDebug.type(text)', 'C:\\HOUSE.H31\r')
            await wait_screen(page, 'discard-reset-prompt',
                              lambda text: 'save changes' in text)
            await keys(page, 'n$10')
            await wait_screen(page, 'open-restores-saved-geometry',
                lambda image: red_geometry(image) == changed_roof, pixels=True)
            assert not errors, errors
            print('PASS: all real Windows 3.1 UI assertions', flush=True)
        finally:
            await browser.close()


if __name__ == '__main__':
    asyncio.run(main())
