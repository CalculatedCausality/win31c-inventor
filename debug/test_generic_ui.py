"""Generic CAD end-to-end tests; --headed --hold makes the run visible."""
import argparse
import asyncio
import re
from playwright.async_api import async_playwright, TimeoutError as PlaywrightTimeout
from test_cad_ui import wait_screen, keys, dialog, recognize, frame, OUTPUT


def has_counts(text, vertices, edges):
    normalized = text.replace('vertlces', 'vertices').replace('edqes', 'edges')
    return (re.search(r'\b' + str(vertices) + r'\s+vertices', normalized) and
            re.search(r'\b' + str(edges) + r'\s+edges', normalized))


async def progress(page, text, visible):
    print('STEP:', text, flush=True)
    await page.evaluate('''text => {
        let node = document.getElementById('test-progress');
        if (!node) {
            node = document.createElement('div'); node.id = 'test-progress';
            node.style.cssText = 'position:fixed;bottom:0;left:0;right:0;z-index:999999;background:#164a32;color:white;padding:10px;font:15px system-ui;pointer-events:none';
            document.body.appendChild(node);
        }
        node.textContent = 'AUTOMATED UI TEST — ' + text;
    }''', text)
    if visible:
        await page.bring_to_front()
        await page.wait_for_timeout(2000)


async def counts(page, name, vertices, edges):
    return await wait_screen(page, name, lambda text: has_counts(text, vertices, edges), timeout=70,
                             reader=status_text)


def status_text(image):
    data = rendered_state(image)
    if data:
        return (f"{data['vertices']} vertices / {data['edges']} edges | "
                f"{'shaded' if data['shaded'] else 'wireframe'}")
    strip = image.crop((3, image.height - 17, image.width - 3, image.height - 2)).convert('L')
    strip = strip.point(lambda value: 255 if value > 140 else 0)
    strip = strip.resize((strip.width * 2, strip.height * 2))
    return recognize(strip, line=True)


def rendered_state(image):
    """Decode checksummed /UITEST pixels emitted after the actual GDI paint."""
    pixels = image.convert('RGB').load()
    magic = ((255, 0, 255), (255, 255, 0), (0, 255, 255), (255, 0, 255))
    # VGA DAC channels are six-bit: requested 255 is rendered as 252.
    def same_color(actual, expected):
        return max(abs(a - b) for a, b in zip(actual, expected)) <= 5
    for y in range(image.height - 2, max(-1, image.height - 80), -1):
        for x in range(max(0, image.width - 70)):
            if any(not same_color(pixels[x + i * 2, y], color) or not same_color(pixels[x + i * 2, y + 1], color)
                   for i, color in enumerate(magic)):
                continue
            encoded = 0
            valid = True
            for bit in range(32):
                color = pixels[x + (4 + bit) * 2, y]
                white = same_color(color, (255, 255, 255))
                black = same_color(color, (0, 0, 0))
                if not (white or black) or pixels[x + (4 + bit) * 2, y + 1] != color:
                    valid = False
                    break
                if white:
                    encoded |= 1 << bit
            low = encoded & 0xffffff
            if not valid or encoded >> 24 != ((low ^ (low >> 8) ^ (low >> 16) ^ 0xa7) & 255):
                continue
            return {'vertices': low & 511, 'edges': (low >> 9) & 1023,
                    'units': (low >> 19) & 3, 'shaded': bool(low & (1 << 21)),
                    'dirty': bool(low & (1 << 22))}
    return None


def cube_region(image):
    # ViewCube sits in the top-right corner of the canvas, below the ribbon.
    return image.crop((image.width - 140, 105, image.width - 4, 245)).convert('L')


def cube_difference(a, b):
    pa, pb = cube_region(a).tobytes(), cube_region(b).tobytes()
    return sum(abs(x - y) > 40 for x, y in zip(pa, pb))


def cube_asymmetry(image):
    """Front view shows one square face: the cube is left-right symmetric."""
    region = cube_region(image)
    w, h = region.size
    data = region.load()
    return sum(abs(data[x, y] - data[w - 1 - x, y]) > 40
               for y in range(h) for x in range(w // 2))


async def settled(page, timeout=90):
    """Frame whose telemetry (drawn last) is present and unchanged twice."""
    loop = asyncio.get_event_loop()
    deadline = loop.time() + timeout
    previous = None
    while loop.time() < deadline:
        image = await frame(page)
        if rendered_state(image) and previous is not None and image.tobytes() == previous.tobytes():
            return image
        previous = image if rendered_state(image) else None
        await page.wait_for_timeout(1500)
    raise AssertionError('guest frame never settled')


def cube_drawn(image):
    return sum(v < 200 for v in cube_region(image).tobytes()) > 300


def surface_pixels(image):
    # Only the guest's modeling viewport: exclude ribbon, browser, cube and status.
    region = image.crop((182, 108, image.width - 145, image.height - 28))
    data = region.convert('RGB').tobytes()
    return sum(min(data[i:i + 3]) < 235 for i in range(0, len(data), 3))


async def main(headed=False, hold=False, windows=False):
    OUTPUT.mkdir(parents=True, exist_ok=True)
    async with async_playwright() as p:
        if windows:
            from windows_bridge import WindowsBrowser
            browser = await WindowsBrowser.connect()
            headed = True
        else:
            browser = await p.chromium.launch(headless=not headed)
        page = await browser.new_page(viewport={'width': 1100, 'height': 850})
        errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))
        try:
            await page.goto('http://localhost:8310/')
            await page.evaluate("document.title = 'win31c — CAD visible test'")
            if headed:
                session = await page.context.new_cdp_session(page)
                window = await session.send('Browser.getWindowForTarget')
                await session.send('Browser.setWindowBounds', {'windowId': window['windowId'], 'bounds': {'windowState': 'normal'}})
                await session.send('Browser.setWindowBounds', {'windowId': window['windowId'], 'bounds': {'left': 40, 'top': 40, 'width': 1180, 'height': 950}})
                await page.bring_to_front()
            await progress(page, 'Boot Windows 3.1', headed)
            await page.wait_for_function('cadDebug.status().ready', timeout=90000)
            await page.evaluate('cadDebug.speed()')
            await page.evaluate('cadDebug.speed()')
            await page.evaluate('cadDebug.mountLatest()')
            await wait_screen(page, 'generic-windows-boot', lambda text: 'program manager' in text, title=True, timeout=90)
            await progress(page, 'Launch CAD.EXE — verify empty startup', headed)
            await page.evaluate('cadDebug.launchCAD({uiTest: true})')
            await wait_screen(page, 'generic-caption', lambda text: 'cad' in text and 'untitled' in text, title=True, timeout=100)
            await keys(page, '$altt$10d$10')
            await counts(page, 'generic-empty-startup', 0, 0)
            await progress(page, 'Create a box in the empty document', headed)
            await keys(page, '$altc$10b$10')
            await wait_screen(page, 'generic-box-dialog', lambda text: 'create box' in text, title=True)
            await keys(page, '$enter$10')
            await counts(page, 'generic-box-created', 8, 12)
            await progress(page, 'Add a separate line to the same document', headed)
            await keys(page, '$altc$10l$10')
            await wait_screen(page, 'generic-line-dialog', lambda text: 'create line' in text, title=True)
            await keys(page, '$enter$10')
            await counts(page, 'generic-multiple-primitives', 10, 13)
            await progress(page, 'Undo and redo the line creation', headed)
            await keys(page, '$ctrlz$10')
            await counts(page, 'generic-undo', 8, 12)
            await keys(page, '$ctrly$10')
            await counts(page, 'generic-redo', 10, 13)
            await progress(page, 'Save arbitrary geometry to C:\\TEST.CAD', headed)
            await keys(page, '$ctrls$10')
            await dialog(page, 'generic-save-dialog')
            await page.evaluate('(text) => cadDebug.type(text)', 'C:\\TEST.CAD\r')
            await wait_screen(page, 'generic-saved-caption', lambda text: 'test.cad' in text, title=True)
            await keys(page, '$ctrln$10')
            await counts(page, 'generic-new-document', 0, 0)
            await progress(page, 'Reopen the saved box and line', headed)
            await keys(page, '$ctrlo$10')
            await dialog(page, 'generic-open-dialog')
            await page.evaluate('(text) => cadDebug.type(text)', 'C:\\TEST.CAD\r')
            await counts(page, 'generic-save-load-roundtrip', 10, 13)
            await progress(page, 'Load the optional House demo from Demos', headed)
            await keys(page, '$altd$10h$10')
            await counts(page, 'generic-optional-house-demo', 30, 40)
            await keys(page, '$ctrln$10')
            await wait_screen(page, 'generic-discard-demo', lambda text: 'save changes' in text)
            await keys(page, 'n$10')
            await counts(page, 'generic-empty-after-demo', 0, 0)
            await progress(page, 'Load Boeing 747-400 demo in shaded mode', headed)
            await keys(page, '$altd$10b$10')
            shaded_image = await counts(page, 'aircraft-loaded', 236, 396)
            await wait_screen(page, 'aircraft-caption', lambda text: '747' in text, title=True)
            shaded_count = surface_pixels(shaded_image)
            await progress(page, 'Switch the aircraft to wireframe', headed)
            await keys(page, '$altv$10w$10')
            wire_image = await wait_screen(page, 'aircraft-wireframe',
                                          lambda text: 'wireframe' in text, reader=status_text)
            wire_count = surface_pixels(wire_image)
            assert shaded_count > wire_count * 1.25, (shaded_count, wire_count)
            await progress(page, 'Restore shaded mode; camera and geometry stay unchanged', headed)
            await keys(page, '$altv$10h$10')
            await wait_screen(page, 'aircraft-shaded-restored', lambda text: 'shaded' in text, reader=status_text)
            await counts(page, 'aircraft-geometry-preserved', 236, 396)
            iso_image = await settled(page)
            assert cube_drawn(iso_image), 'ViewCube not drawn'
            await progress(page, 'ViewCube: View > Front re-orients the orbit cube', headed)
            iso_asym = cube_asymmetry(iso_image)
            await keys(page, '$altv$10f$10')
            await page.wait_for_timeout(3000)
            front_image = await settled(page)
            assert cube_drawn(front_image), 'ViewCube not drawn in front view'
            front_image.save(OUTPUT / 'viewcube-front.png')
            changed = cube_difference(iso_image, front_image)
            front_asym = cube_asymmetry(front_image)
            print(f'viewcube: changed={changed} iso_asym={iso_asym} front_asym={front_asym}', flush=True)
            assert changed > 150 and front_asym * 2 < max(iso_asym, 1), (changed, iso_asym, front_asym)
            print('PASS: viewcube-front', flush=True)
            await keys(page, '$altv$10i$10')
            await page.wait_for_timeout(3000)
            back_image = await settled(page)
            back_image.save(OUTPUT / 'viewcube-iso.png')
            assert cube_difference(iso_image, back_image) < 80, cube_difference(iso_image, back_image)
            print('PASS: viewcube-isometric-restored', flush=True)
            assert not errors, errors
            await progress(page, 'PASS — generic CAD, Boeing 747 shaded/wireframe and ViewCube tests. You can use CAD now.', False)
            print('PASS: generic CAD guest UI tests', flush=True)
        except Exception as error:
            await progress(page, 'FAILED: ' + str(error), False)
            (await frame(page)).save(OUTPUT / 'generic-failure.png')
            raise
        finally:
            if headed and hold and not page.is_closed():
                print('Visible test window remains open for 15 minutes, or until you close it.', flush=True)
                try:
                    await page.wait_for_event('close', timeout=900000)
                except PlaywrightTimeout:
                    pass
            await browser.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--headed', action='store_true')
    parser.add_argument('--hold', action='store_true')
    parser.add_argument('--windows', action='store_true', help='Drive native Windows Edge on port 9223')
    options = parser.parse_args()
    asyncio.run(main(options.headed, options.hold, options.windows))
