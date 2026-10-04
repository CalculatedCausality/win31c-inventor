"""Exercise native pointer capture at different display sizes."""
from playwright.sync_api import sync_playwright


def main():
    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)
        page = browser.new_page(viewport={"width": 1280, "height": 1000})
        errors = []
        page.on("pageerror", lambda e: errors.append(str(e)))
        page.goto("http://localhost:8310/")
        page.wait_for_function("window.cadDebug?.status().ready", timeout=90000)
        assert page.locator('#cad .pcjs-controls').count() > 0
        assert page.locator('#cad .pcjs-container').count() > 0
        assert page.evaluate("[...document.querySelectorAll('#cad .pcjs-controls')].every(e => getComputedStyle(e).display === 'none')")
        assert page.evaluate("[...document.querySelectorAll('#cad .pcjs-container')].every(e => getComputedStyle(e).borderTopWidth === '0px' && getComputedStyle(e).borderRadius === '0px')")
        # Explicit capture is a trusted button gesture, even before guest boot.
        for width in (1280, 720):
            page.set_viewport_size({"width": width, "height": 1000})
            page.click('[data-action="captureMouse"]')
            page.wait_for_function("document.pointerLockElement !== null")
            assert page.evaluate("cadDebug.status().mouseCaptured")
            assert page.evaluate("document.pointerLockElement.classList.contains('pcjs-overlay')")
            # Captured relative movement must leave the pointer locked.
            page.mouse.move(400, 400)
            page.mouse.move(420, 410)
            assert page.evaluate("cadDebug.status().mouseCaptured")
            page.evaluate("cadDebug.releaseMouse()")
            page.wait_for_function("document.pointerLockElement === null")
            page.evaluate("cadDebug.focus()")
            assert page.evaluate("document.activeElement.classList.contains('pcjs-overlay')")
            assert not page.evaluate("cadDebug.status().mouseCaptured")
        assert not errors, errors
        browser.close()
    print("PASS: bezel-free display, native mouse capture/release, relative movement, resized display, keyboard-only focus")


if __name__ == "__main__":
    main()
