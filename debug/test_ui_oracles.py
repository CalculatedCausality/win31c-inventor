"""Check the UI test oracles reject absent/occluded geometry and blank captions."""
import unittest
from PIL import Image, ImageDraw, ImageFont
from test_cad_ui import recognize, red_geometry, white_fraction
from test_generic_ui import (rendered_state, feature_state, STATE_MAGIC, FEATURE_MAGIC, surface_pixels,
                             geometry_box, feature_pixels, CANVAS_TOP, SILVER,
                             render_state, RENDER_MAGIC)


class OracleTests(unittest.TestCase):
    def test_guest_state_marker_is_exact_and_rejects_corruption(self):
        image = Image.new('RGB', (640, 480), 'white')
        pixels = image.load()
        assert pixels is not None
        payload = 236 | (396 << 9) | (1 << 21) | (1 << 22)
        checksum = (payload ^ (payload >> 8) ^ (payload >> 16) ^ 0xa7) & 255
        encoded = payload | (checksum << 24)
        colors = [(255, 0, 255), (255, 255, 0), (0, 255, 255), (255, 0, 255)]
        colors += [(255, 255, 255) if encoded & (1 << bit) else (0, 0, 0) for bit in range(32)]
        for row in (473, 474):
            for index, color in enumerate(colors):
                pixels[6 + index * 2, row] = color
        state = rendered_state(image)
        assert state is not None
        self.assertEqual((state['vertices'], state['edges']), (236, 396))
        self.assertTrue(state['shaded'] and state['dirty'])
        self.assertFalse(state['components'])
        pixels[14, 473] = (255, 255, 255)  # Torn row / incorrect payload.
        self.assertIsNone(rendered_state(image))
        pixels[14, 474] = (255, 255, 255)  # Both rows agree, checksum still wrong.
        self.assertIsNone(rendered_state(image))

    def test_guest_state_marker_reports_placed_components(self):
        image = Image.new('RGB', (640, 480), 'white')
        pixels = image.load()
        assert pixels is not None
        payload = 2 | (1 << 9) | (1 << 22) | (1 << 23)
        checksum = (payload ^ (payload >> 8) ^ (payload >> 16) ^ 0xa7) & 255
        encoded = payload | (checksum << 24)
        colors = [(255, 0, 255), (255, 255, 0), (0, 255, 255), (255, 0, 255)]
        colors += [(255, 255, 255) if encoded & (1 << bit) else (0, 0, 0) for bit in range(32)]
        for row in (473, 474):
            for index, color in enumerate(colors):
                pixels[6 + index * 2, row] = color
        state = rendered_state(image)
        assert state is not None
        self.assertEqual((state['vertices'], state['edges']), (2, 1))
        self.assertTrue(state['components'] and state['dirty'] and not state['shaded'])

    def test_feature_strip_is_separate_from_the_state_strip(self):
        image = Image.new('RGB', (640, 480), 'white')
        pixels = image.load()
        assert pixels is not None
        def strip(row, magic, payload, constant):
            checksum = (payload ^ (payload >> 8) ^ (payload >> 16) ^ constant) & 255
            encoded = payload | (checksum << 24)
            colors = list(magic) + [(255, 255, 255) if encoded & (1 << bit) else (0, 0, 0) for bit in range(32)]
            for y in (row, row + 1):
                for index, color in enumerate(colors):
                    pixels[6 + index * 2, y] = color
        # 2 features, 1 failed, sketch mode, X extent 200, bounds.
        feature = 2 | (1 << 5) | (1 << 10) | (200 << 12) | (1 << 23)
        strip(473, STATE_MAGIC, 8 | (12 << 9), 0xa7)
        strip(475, FEATURE_MAGIC, feature, 0x5c)
        state, features = rendered_state(image), feature_state(image)
        assert state is not None and features is not None
        self.assertEqual((state['vertices'], state['edges']), (8, 12))
        self.assertEqual((features['features'], features['errors'], features['extent_x']), (2, 1, 200))
        self.assertTrue(features['sketch_mode'] and features['bounds'] and not features['shaded'])
        # The feature checksum constant differs: a state-style word is rejected.
        strip(475, FEATURE_MAGIC, feature, 0xa7)
        self.assertIsNone(feature_state(image))
        self.assertIsNotNone(rendered_state(image))

    def test_gradient_background_is_not_surface(self):
        """The dithered white/silver viewport gradient and silver grid lines
        must not count as shaded surface; real faces still do."""
        image = Image.new('RGB', (640, 480), (252, 252, 252))
        pixels = image.load()
        assert pixels is not None
        for y in range(CANVAS_TOP + 150, 456):
            for x in range(155, 640):
                if (x + y) % 3 == 0:
                    pixels[x, y] = SILVER
        self.assertEqual(surface_pixels(image), 0)
        ImageDraw.Draw(image).rectangle((250, 250, 349, 299), fill=(132, 136, 140))
        self.assertEqual(surface_pixels(image), 100 * 50)

    def test_viewport_chrome_is_not_geometry(self):
        """ViewCube, Home button and navigation bar pixels never count as
        model geometry; a line in the free viewport does."""
        image = Image.new('RGB', (640, 480), (252, 252, 252))
        draw = ImageDraw.Draw(image)
        draw.rectangle((640 - 130, CANVAS_TOP + 8, 640 - 10, CANVAS_TOP + 128), outline='black')
        draw.rectangle((640 - 174, CANVAS_TOP + 8, 640 - 144, CANVAS_TOP + 38), fill='black')
        draw.rectangle((640 - 30, CANVAS_TOP + 140, 640 - 6, CANVAS_TOP + 230), fill='black')
        self.assertIsNone(geometry_box(image))
        self.assertEqual(feature_pixels(image), 0)
        draw.line((200, 300, 299, 300), fill='black')
        self.assertEqual(geometry_box(image), (100, 1))
        self.assertEqual(feature_pixels(image), 100)

    def test_render_strip_is_separate_and_checksummed(self):
        image = Image.new('RGB', (640, 480), 'white')
        pixels = image.load()
        assert pixels is not None
        payload = 1234 | (1 << 16)
        checksum = (payload ^ (payload >> 8) ^ (payload >> 16) ^ 0x3e) & 255
        encoded = payload | (checksum << 24)
        colors = list(RENDER_MAGIC) + [(255, 255, 255) if encoded & (1 << b) else (0, 0, 0) for b in range(32)]
        for y in (465, 466):
            for index, color in enumerate(colors):
                pixels[6 + index * 2, y] = color
        self.assertEqual(render_state(image), {'renders': 1234, 'cached': True, 'pending': False})
        self.assertIsNone(rendered_state(image))
        pixels[14, 465] = pixels[14, 466] = (0, 0, 0) if encoded & 1 else (255, 255, 255)
        self.assertIsNone(render_state(image))

    def test_dialog_occlusion_is_not_a_successful_edit(self):
        original = Image.new('RGB', (400, 300), 'black')
        ImageDraw.Draw(original).line((40, 180, 200, 30, 350, 180), fill='red', width=2)
        before = red_geometry(original)
        occluded = original.copy()
        ImageDraw.Draw(occluded).rectangle((0, 0, 400, 200), fill='white')
        # A dialog can remove roof pixels, but cannot create the new red edges
        # required by the macro-success assertion.
        self.assertGreater(len(before.symmetric_difference(red_geometry(occluded))), 200)
        self.assertEqual(len(red_geometry(occluded) - before), 0)

    def test_cursor_and_caption_do_not_change_geometry_signature(self):
        image = Image.new('RGB', (300, 200), 'black')
        draw = ImageDraw.Draw(image)
        draw.line((10, 120, 150, 20, 290, 120), fill='red')
        before = red_geometry(image)
        draw.rectangle((10, 150, 200, 180), fill='blue')
        draw.line((240, 160, 260, 160), fill='white')
        self.assertEqual(red_geometry(image), before)

    def test_blank_screen_has_no_active_window_caption(self):
        self.assertEqual(recognize(Image.new('RGB', (640, 480), 'black'), title=True), '')

    def test_caption_is_not_confused_with_wider_blue_dialog_border(self):
        image = Image.new('RGB', (640, 480), 'white')
        draw = ImageDraw.Draw(image)
        draw.rectangle((0, 0, 600, 350), outline='blue', width=3)
        draw.rectangle((24, 20, 597, 48), fill='blue')
        draw.text((160, 24), 'Create box', font=ImageFont.truetype('DejaVuSans.ttf', 20), fill='white')
        self.assertIn('create box', recognize(image, title=True))

    def test_paper_detector_distinguishes_black_viewport(self):
        self.assertEqual(white_fraction(Image.new('RGB', (100, 100), 'black')), 0)
        self.assertEqual(white_fraction(Image.new('RGB', (100, 100), 'white')), 1)


if __name__ == '__main__':
    unittest.main()
