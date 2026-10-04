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


# Guest layout (640x480, maximized): caption + menu bar end at y=40, the
# ribbon is 92 rows (cad_chrome.h), so the viewport starts at CANVAS_TOP.
# Right of the viewport: ViewCube (top), Home button left of it and the
# navigation bar below it; VIEW_RIGHT excludes that column from geometry.
CANVAS_TOP = 40 + 92
VIEW_RIGHT = 180
# Background dither / grid colour: VGA silver as the guest renders it.
SILVER = (192, 196, 200)


def chrome_rects(image):
    """Viewport chrome drawn over the scene: ViewCube, Home, navigation bar."""
    w = image.width
    return [(w - 142, CANVAS_TOP, w, CANVAS_TOP + 140),
            (w - 180, CANVAS_TOP + 2, w - 138, CANVAS_TOP + 44),
            (w - 36, CANVAS_TOP + 134, w, CANVAS_TOP + 240)]


STATE_MAGIC = ((255, 0, 255), (255, 255, 0), (0, 255, 255), (255, 0, 255))
FEATURE_MAGIC = ((0, 255, 255), (255, 0, 255), (255, 255, 0), (0, 255, 255))


def rendered_state(image):
    """Decode checksummed /UITEST pixels emitted after the actual GDI paint."""
    low = telemetry_word(image, STATE_MAGIC, 0xa7)
    if low is None:
        return None
    return {'vertices': low & 511, 'edges': (low >> 9) & 1023,
            'units': (low >> 19) & 3, 'shaded': bool(low & (1 << 21)),
            'dirty': bool(low & (1 << 22)),
            'components': bool(low & (1 << 23))}


def feature_state(image):
    """Second /UITEST strip (cad_fui_telemetry): the part feature tree."""
    low = telemetry_word(image, FEATURE_MAGIC, 0x5c)
    if low is None:
        return None
    return {'features': low & 31, 'errors': (low >> 5) & 31,
            'sketch_mode': bool(low & (1 << 10)), 'shaded': bool(low & (1 << 11)),
            'extent_x': (low >> 12) & 1023, 'truncated': bool(low & (1 << 22)),
            'bounds': bool(low & (1 << 23))}


def telemetry_word(image, magic, constant):
    """24-bit payload of a strip: 4 marker pixels then 32 bits, two rows high."""
    pixels = image.convert('RGB').load()
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
            if not valid or encoded >> 24 != ((low ^ (low >> 8) ^ (low >> 16) ^ constant) & 255):
                continue
            return low
    return None


RENDER_MAGIC = ((255, 255, 0), (0, 255, 255), (255, 0, 255), (255, 255, 0))


def render_state(image):
    """Third /UITEST strip (status rows 5-6): scene renders, cache, pending."""
    low = telemetry_word(image, RENDER_MAGIC, 0x3e)
    if low is None:
        return None
    return {'renders': low & 0xffff, 'cached': bool(low & (1 << 16)), 'pending': bool(low & (1 << 17))}


def ribbon_tint(image):
    """Cyan pixels in the ribbon: a few in icon artwork at rest; a hovered
    button's tint (2/16 cyan dither) adds about a hundred or more."""
    data = image.crop((0, 40, image.width, 40 + 92)).convert('RGB').tobytes()
    return sum(data[i] < 20 and data[i + 1] > 240 and data[i + 2] > 240 for i in range(0, len(data), 3))


async def hover_sweep(page):
    """Hover must never render the scene. Guest mouse input is relative
    (about half a guest pixel per browser pixel) and its position cannot be
    read back, so: climb to the top edge (moving up inside the canvas and
    returning outside it, where the emulator sees no events), then sweep the
    screen in zig-zag rows over ribbon, tabs, ViewCube, Home and nav bar."""
    box = await page.evaluate("(() => { const r = document.querySelector('#cad canvas').getBoundingClientRect();"
                              " return [r.left, r.top, r.width, r.height]; })()")
    left, top, width, height = box
    async def path(points, step=4):
        x, y = points[0]
        await page.mouse.move(x, y)
        for nx, ny in points[1:]:
            n = int(max(abs(nx - x), abs(ny - y)) // step) or 1
            for i in range(1, n + 1):
                await page.mouse.move(x + (nx - x) * i / n, y + (ny - y) * i / n)
                await page.wait_for_timeout(8)
            x, y = nx, ny
    before = await settled(page)
    start = render_state(before)
    assert start, 'render telemetry missing'
    middle = left + width / 2
    for _ in range(3):
        await path([(middle, top + height + 8), (middle, top + height - 2), (middle, top + 2), (middle, top - 8)])
        await path([(middle, top - 8), (left - 8, top - 8), (left - 8, top + height + 8), (middle, top + height + 8)], 40)
    await path([(left - 8, top + 2), (left + 2, top + 2)])
    baseline = ribbon_tint(before)
    tinted, x = 0, left + 2
    for row in range(14):
        y = top + 2 + row * 44
        target = left + width - 2 if row % 2 == 0 else left + 2
        for k in range(1, 5):
            await path([(x, y), (x + (target - x) * k / 4, y)])
            image = await frame(page)
            tinted += ribbon_tint(image) > baseline + 40
            state = render_state(image)
            # A frame grabbed mid status-bar repaint has no strip; skip it.
            assert state is None or state['renders'] == start['renders'], (start, state)
        x = target
        await path([(x, y), (x, y + 44)])
    after = await settled(page)
    after.save(OUTPUT / 'hover-sweep-end.png')
    end = render_state(after)
    print(f"hover: renders {start['renders']} -> {end['renders']}, scene cache={end['cached']}, "
          f"frames with ribbon hover tint={tinted}", flush=True)
    assert end['renders'] == start['renders'], (start, end)
    # Non-vacuous: the sweep must actually have produced hover feedback.
    assert tinted >= 1, 'the sweep never hovered a ribbon button'

    return end


def geometry_box(image):
    """Bounding box (width, height) of geometry-pen pixels in the viewport,
    skipping the ViewCube corner; None if nothing is drawn."""
    rgb = image.convert('RGB')
    pixels = rgb.load()
    xs, ys = [], []
    skip = chrome_rects(image)
    for y in range(CANVAS_TOP + 4, image.height - 28):
        for x in range(162, image.width - 4):
            if any(x0 <= x < x1 and y0 <= y < y1 for x0, y0, x1, y1 in skip):
                continue
            if min(pixels[x, y]) < 120:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return max(xs) - min(xs) + 1, max(ys) - min(ys) + 1


def feature_pixels(image):
    """Viewport pixels drawn by geometry pens (black body edges, blue sketch
    curves, red selection, shaded faces); the light grid and dimmed lines are
    excluded."""
    region = image.crop((182, CANVAS_TOP + 4, image.width - VIEW_RIGHT, image.height - 28))
    data = region.convert('RGB').tobytes()
    return sum(min(data[i:i + 3]) < 120 for i in range(0, len(data), 3))


def cube_region(image):
    # ViewCube sits in the top-right corner of the canvas, below the ribbon.
    return image.crop((image.width - 140, CANVAS_TOP + 1, image.width - 4, CANVAS_TOP + 136)).convert('L')


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
        # The scene renders off-screen: wait until no render is pending.
        ready = rendered_state(image) and not (render_state(image) or {}).get('pending')
        if ready and previous is not None and image.tobytes() == previous.tobytes():
            return image
        previous = image if ready else None
        await page.wait_for_timeout(1500)
    raise AssertionError('guest frame never settled')


def cube_drawn(image):
    return sum(v < 200 for v in cube_region(image).tobytes()) > 300


def surface_pixels(image):
    # Only the guest's modeling viewport: exclude ribbon, browser, cube and
    # status. The background gradient and the grid are white/silver only.
    region = image.crop((182, CANVAS_TOP + 4, image.width - VIEW_RIGHT, image.height - 28))
    data = region.convert('RGB').tobytes()
    return sum(min(data[i:i + 3]) < 235 and tuple(data[i:i + 3]) != SILVER for i in range(0, len(data), 3))


def component_pixels(image):
    """Placed components: dark blue (0,0,160 -> VGA 0,0,168) or, when
    selected, the 2px red selection pen. Empty documents draw neither."""
    region = image.crop((182, CANVAS_TOP + 4, image.width - VIEW_RIGHT, image.height - 28))
    data = region.convert('RGB').tobytes()
    count = 0
    for i in range(0, len(data), 3):
        r, g, b = data[i], data[i + 1], data[i + 2]
        if (b > 100 and r < 60 and g < 60) or (r > 120 and g < 80 and b < 80):
            count += 1
    return count


async def state_is(page, name, predicate, timeout=70):
    return await wait_screen(page, name, lambda text: text == 'yes', timeout=timeout,
                             reader=lambda image: 'yes' if predicate(rendered_state(image) or {}, image) else 'no')


async def features_are(page, name, predicate, timeout=70):
    """Wait for a settled frame whose feature telemetry satisfies predicate."""
    return await wait_screen(page, name, lambda text: text == 'yes', timeout=timeout,
                             reader=lambda image: 'yes' if predicate(feature_state(image) or {}, image) else 'no')


async def modify_parameter(page, name, value):
    """Parameters (fx): select the first row, replace its expression, Modify."""
    await keys(page, '$altt$10p$10')
    await wait_screen(page, f'{name}-dialog', lambda text: 'parameters' in text, title=True)
    await page.wait_for_timeout(2500)
    # Name, Expression, Add, Modify, Delete -> list; Down selects row 1.
    await keys(page, '$tab$10$tab$10$tab$10$tab$10$tab$10')
    await page.wait_for_timeout(1000)
    await keys(page, '$down$10')
    await page.wait_for_timeout(1500)
    await keys(page, '$alte$10')
    await page.wait_for_timeout(1000)
    await page.evaluate('(text) => cadDebug.type(text)', value)
    await page.wait_for_timeout(1000)
    await keys(page, '$altm$10')
    await page.wait_for_timeout(2500)
    (await frame(page)).save(OUTPUT / f'{name}-modified.png')
    await keys(page, '$esc$10')


CHROME_TABS = ('model', 'sketch', 'assemble', 'constrain', 'manage', 'view')


async def chrome_screens(page, headed, prefix):
    """View > Ribbon tab > 1..6: screenshot every ribbon tab for review."""
    await progress(page, 'Ribbon tabs: chrome screenshots', headed)
    images = []
    for index, name in enumerate(CHROME_TABS):
        await keys(page, '$altv$10r$10')
        await page.evaluate('(text) => cadDebug.type(text)', str(index + 1))
        await page.wait_for_timeout(2500)
        image = await settled(page)
        image.save(OUTPUT / f'chrome-{prefix}-{name}.png')
        images.append(image)
    # Each tab paints a different ribbon body.
    bodies = [image.crop((0, 40 + 23, image.width, 40 + 77)).tobytes() for image in images]
    assert len(set(bodies)) == len(bodies), 'ribbon tabs did not repaint'
    await keys(page, '$altv$10r$10')
    await page.evaluate('(text) => cadDebug.type(text)', '1')
    await page.wait_for_timeout(2000)


async def part_feature_steps(page, headed):
    """Inventor-style part: W=100, sketch rectangle W x 60, extrude 40,
    then W=200 must widen the body (associativity); save/reopen."""
    await progress(page, 'Part features: Parameters W = 100', headed)
    await keys(page, '$altt$10p$10')
    await wait_screen(page, 'feat-params-dialog', lambda text: 'parameters' in text, title=True)
    await page.wait_for_timeout(2500)
    await page.evaluate('(text) => cadDebug.type(text)', 'W')
    await page.wait_for_timeout(1000)
    await keys(page, '$tab$10')
    await page.wait_for_timeout(1000)
    await page.evaluate('(text) => cadDebug.type(text)', '100')
    await page.wait_for_timeout(1000)
    await keys(page, '$enter$10')
    await page.wait_for_timeout(2500)
    await keys(page, '$esc$10')
    await state_is(page, 'feat-params-dirty', lambda state, image: state.get('dirty'))
    await progress(page, 'Part > Start 2D Sketch on XY', headed)
    await keys(page, '$altp$10s$10')
    await wait_screen(page, 'feat-sketch-dialog', lambda text: 'sketch' in text, title=True)
    await page.wait_for_timeout(2500)
    await keys(page, '$enter$10')
    await features_are(page, 'feat-sketch-mode', lambda f, image: f.get('features') == 1 and f.get('sketch_mode'))
    await progress(page, 'Rectangle: center 0,0, width W, height 60', headed)
    await keys(page, '$altp$10r$10')
    await wait_screen(page, 'feat-rect-dialog', lambda text: 'rectangle' in text, title=True)
    await page.wait_for_timeout(2500)
    await keys(page, '$tab$10$tab$10')
    await page.wait_for_timeout(1000)
    await page.evaluate('(text) => cadDebug.type(text)', 'W')
    await page.wait_for_timeout(1000)
    await keys(page, '$tab$10')
    await page.wait_for_timeout(1000)
    await page.evaluate('(text) => cadDebug.type(text)', '60')
    await page.wait_for_timeout(1000)
    await keys(page, '$enter$10')
    await features_are(page, 'feat-rectangle', lambda f, image: f.get('bounds') and f.get('extent_x') == 100)
    # Entering sketch mode switched the ribbon to the Sketch tab.
    (await settled(page)).save(OUTPUT / 'chrome-sketch-mode.png')
    await progress(page, 'Finish Sketch, then Extrude 40', headed)
    await keys(page, '$altp$10f$10')
    await features_are(page, 'feat-finished', lambda f, image: f.get('features') == 1 and not f.get('sketch_mode'))
    await keys(page, '$altp$10e$10')
    await wait_screen(page, 'feat-extrude-dialog', lambda text: 'extrude' in text, title=True)
    await page.wait_for_timeout(2500)
    await page.evaluate('(text) => cadDebug.type(text)', '40')
    await page.wait_for_timeout(1000)
    await keys(page, '$enter$10')
    narrow = await features_are(page, 'feat-extruded', lambda f, image: f.get('features') == 2 and
                                f.get('errors') == 0 and f.get('extent_x') == 100 and feature_pixels(image) > 300)
    narrow = await settled(page)
    narrow_pixels = feature_pixels(narrow)
    print(f'features: extruded body pixels={narrow_pixels}', flush=True)
    # View > Front zooms to fit: the body's on-screen aspect is W:60.
    await keys(page, '$altv$10f$10')
    await page.wait_for_timeout(3000)
    front = await settled(page)
    front.save(OUTPUT / 'feat-front-w100.png')
    narrow_box = geometry_box(front)
    assert narrow_box, 'no body drawn in the front view'
    await keys(page, '$altv$10i$10')
    await page.wait_for_timeout(3000)
    await progress(page, 'Shaded view of the extrusion', headed)
    await keys(page, '$altv$10h$10')
    shaded = await features_are(page, 'feat-shaded', lambda f, image: f.get('shaded') and
                                (rendered_state(image) or {}).get('shaded'))
    shaded = await settled(page)
    print(f'features: shaded pixels={surface_pixels(shaded)} wire={surface_pixels(narrow)}', flush=True)
    await chrome_screens(page, headed, 'shaded')
    # Filled faces cover far more of the viewport than the 12 body edges.
    assert surface_pixels(shaded) - surface_pixels(narrow) > 3000, (surface_pixels(shaded), surface_pixels(narrow))
    await keys(page, '$altv$10w$10')
    await features_are(page, 'feat-wireframe', lambda f, image: not f.get('shaded') and
                       not (rendered_state(image) or {}).get('shaded'))
    await progress(page, 'Parameters: W = 200 widens the extrusion', headed)
    await modify_parameter(page, 'feat-w200', '200')
    await features_are(page, 'feat-wider', lambda f, image: f.get('extent_x') == 200 and f.get('errors') == 0)
    wide = await settled(page)
    wide.save(OUTPUT / 'feat-iso-w200.png')
    await keys(page, '$altv$10f$10')
    await page.wait_for_timeout(3000)
    front = await settled(page)
    front.save(OUTPUT / 'feat-front-w200.png')
    wide_box = geometry_box(front)
    assert wide_box, 'no body drawn in the front view'
    narrow_aspect = narrow_box[0] / narrow_box[1]
    wide_aspect = wide_box[0] / wide_box[1]
    print(f'features: front-view body box W=100 {narrow_box} (aspect {narrow_aspect:.2f}) -> '
          f'W=200 {wide_box} (aspect {wide_aspect:.2f})', flush=True)
    assert wide_aspect > narrow_aspect * 1.5, (narrow_box, wide_box)
    await keys(page, '$altv$10i$10')
    await page.wait_for_timeout(3000)
    await progress(page, 'Parameters: W = 1000 puts Sketch1 in error, Extrusion1 blocked', headed)
    await modify_parameter(page, 'feat-w1000', '1000')
    await features_are(page, 'feat-error', lambda f, image: f.get('errors') == 2 and not f.get('bounds'))
    failed = await settled(page)
    failed.save(OUTPUT / 'chrome-browser-error.png')
    # Diagnostic only: the selected (inverted) row is not OCR-readable.
    top = CANVAS_TOP + 22
    browser_rows = failed.crop((0, top, 156, failed.height - 30)).convert('L').resize((312, (failed.height - 30 - top) * 2))
    print('features: browser OCR in error state:', repr(recognize(browser_rows)), flush=True)
    await modify_parameter(page, 'feat-w200-again', '200')
    await features_are(page, 'feat-healed', lambda f, image: f.get('errors') == 0 and f.get('extent_x') == 200)
    await progress(page, 'Save C:\\FEAT.CAD, New, reopen: the body comes back', headed)
    await keys(page, '$ctrls$10')
    await dialog(page, 'feat-save-dialog')
    await page.wait_for_timeout(1500)
    await page.evaluate('(text) => cadDebug.type(text)', 'C:\\FEAT.CAD\r')
    await wait_screen(page, 'feat-saved', lambda text: 'feat.cad' in text, title=True)
    await state_is(page, 'feat-clean', lambda state, image: not state.get('dirty'))
    await keys(page, '$ctrln$10')
    await features_are(page, 'feat-new', lambda f, image: f.get('features') == 0 and not f.get('bounds') and
                       feature_pixels(image) * 3 < narrow_pixels)
    await keys(page, '$ctrlo$10')
    await dialog(page, 'feat-open-dialog')
    await page.wait_for_timeout(1500)
    await page.evaluate('(text) => cadDebug.type(text)', 'C:\\FEAT.CAD\r')
    reopened = await features_are(page, 'feat-reopened', lambda f, image: f.get('features') == 2 and
                                  f.get('extent_x') == 200 and f.get('errors') == 0 and feature_pixels(image) > 300)
    assert not (rendered_state(reopened) or {}).get('dirty'), rendered_state(reopened)
    await wait_screen(page, 'feat-reopened-caption', lambda text: 'feat.cad' in text, title=True)
    print(f'features: reopened body pixels={feature_pixels(reopened)}', flush=True)
    await keys(page, '$ctrln$10')
    await counts(page, 'feat-empty-after', 0, 0)


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
            await progress(page, 'Parameters (fx): add W = 120 from the keyboard', headed)
            await keys(page, '$altt$10p$10')
            await wait_screen(page, 'params-dialog', lambda text: 'parameters' in text, title=True)
            # The guest drops keys typed while a dialog is still painting.
            await page.wait_for_timeout(2500)
            await page.evaluate('(text) => cadDebug.type(text)', 'W')
            await page.wait_for_timeout(1000)
            await keys(page, '$tab$10')
            await page.wait_for_timeout(1000)
            await page.evaluate('(text) => cadDebug.type(text)', '120')
            await page.wait_for_timeout(1000)
            await keys(page, '$enter$10')
            # The new row is drawn selected (white on blue), which OCR cannot read;
            # the dirty telemetry below proves the table changed.
            await page.wait_for_timeout(2500)
            (await frame(page)).save(OUTPUT / 'params-added.png')
            await keys(page, '$esc$10')
            # Parameters are document state: the empty document becomes dirty.
            await wait_screen(page, 'params-dirty', lambda text: text == 'dirty',
                              reader=lambda image: 'dirty' if (rendered_state(image) or {}).get('dirty') else 'clean')
            await progress(page, 'Create a box whose Width field is the expression W', headed)
            await keys(page, '$altc$10b$10')
            await wait_screen(page, 'params-box-dialog', lambda text: 'create box' in text, title=True)
            await page.wait_for_timeout(2500)
            await keys(page, '$tab$10$tab$10$tab$10')
            await page.wait_for_timeout(1000)
            await page.evaluate('(text) => cadDebug.type(text)', 'W')
            await page.wait_for_timeout(1000)
            await keys(page, '$enter$10')
            await counts(page, 'params-box-created', 8, 12)
            await progress(page, 'Tools > Run Macro A:\\TABLE.CMD (one undoable group)', headed)
            await keys(page, '$altt$10m$10')
            await dialog(page, 'macro-open-dialog')
            await page.wait_for_timeout(1500)
            await page.evaluate('(text) => cadDebug.type(text)', 'A:\\TABLE.CMD\r')
            table = await counts(page, 'macro-table', 48, 72)
            assert (rendered_state(table) or {}).get('units') == 1, rendered_state(table)
            await keys(page, '$ctrlz$10')
            undone = await counts(page, 'macro-undo-group', 8, 12)
            assert (rendered_state(undone) or {}).get('units') == 0, rendered_state(undone)
            await keys(page, '$ctrln$10')
            await wait_screen(page, 'params-discard', lambda text: 'save changes' in text)
            await keys(page, 'n$10')
            await counts(page, 'generic-empty-after-macro', 0, 0)
            await progress(page, 'Place Component: save a box as C:\\PART.CAD', headed)
            await keys(page, '$altc$10b$10')
            await wait_screen(page, 'parts-box-dialog', lambda text: 'create box' in text, title=True)
            await keys(page, '$enter$10')
            await counts(page, 'parts-box-created', 8, 12)
            await keys(page, '$ctrls$10')
            await dialog(page, 'parts-save-part-dialog')
            await page.wait_for_timeout(1500)
            await page.evaluate('(text) => cadDebug.type(text)', 'C:\\PART.CAD\r')
            await wait_screen(page, 'parts-part-saved', lambda text: 'part.cad' in text, title=True)
            await keys(page, '$ctrln$10')
            empty = await counts(page, 'parts-new-assembly', 0, 0)
            assert not (rendered_state(empty) or {}).get('components'), rendered_state(empty)
            assert component_pixels(empty) < 20, component_pixels(empty)
            await progress(page, 'Insert > Place Component C:\\PART.CAD at X=200 (keyboard)', headed)
            await keys(page, '$alti$10c$10')
            await dialog(page, 'parts-place-open-dialog')
            await page.wait_for_timeout(1500)
            await page.evaluate('(text) => cadDebug.type(text)', 'C:\\PART.CAD\r')
            await wait_screen(page, 'parts-placement-dialog', lambda text: 'place component' in text, title=True)
            await page.wait_for_timeout(2500)
            await page.evaluate('(text) => cadDebug.type(text)', '200')
            await page.wait_for_timeout(1000)
            await keys(page, '$enter$10')
            placed = await state_is(page, 'parts-placed', lambda state, image: state.get('components') and
                                    state.get('dirty') and state.get('vertices') == 0 and component_pixels(image) > 100)
            print(f'parts: placed component pixels={component_pixels(placed)}', flush=True)
            await progress(page, 'Save the assembly as C:\\ASM.CAD, New, reopen it', headed)
            await keys(page, '$ctrls$10')
            await dialog(page, 'parts-save-asm-dialog')
            await page.wait_for_timeout(1500)
            await page.evaluate('(text) => cadDebug.type(text)', 'C:\\ASM.CAD\r')
            await wait_screen(page, 'parts-asm-saved', lambda text: 'asm.cad' in text, title=True)
            await state_is(page, 'parts-asm-clean', lambda state, image: state.get('components') and not state.get('dirty'))
            await keys(page, '$ctrln$10')
            cleared = await state_is(page, 'parts-asm-closed', lambda state, image: state and not state.get('components') and
                                     state.get('vertices') == 0 and component_pixels(image) < 20)
            await keys(page, '$ctrlo$10')
            await dialog(page, 'parts-open-asm-dialog')
            await page.wait_for_timeout(1500)
            await page.evaluate('(text) => cadDebug.type(text)', 'C:\\ASM.CAD\r')
            reopened = await state_is(page, 'parts-asm-reopened', lambda state, image: state.get('components') and
                                      not state.get('dirty') and state.get('vertices') == 0 and component_pixels(image) > 100)
            await wait_screen(page, 'parts-asm-caption', lambda text: 'asm.cad' in text, title=True)
            print(f'parts: reopened component pixels={component_pixels(reopened)} (closed={component_pixels(cleared)})', flush=True)
            await keys(page, '$ctrln$10')
            await counts(page, 'parts-empty-after-asm', 0, 0)
            await part_feature_steps(page, headed)
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
            await progress(page, 'Hover over ribbon, ViewCube and nav bar never re-renders the 747', headed)
            hovered = await hover_sweep(page)
            print('PASS: hover-without-render', flush=True)
            await keys(page, '$altv$10f$10')
            await page.wait_for_timeout(3000)
            front = render_state(await settled(page))
            print(f"camera change: renders {hovered['renders']} -> {front['renders']}", flush=True)
            assert front['renders'] == hovered['renders'] + 1, (hovered, front)
            print('PASS: camera-change-renders-once', flush=True)
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
