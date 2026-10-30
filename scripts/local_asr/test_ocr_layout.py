import unittest
import contextlib
from pathlib import Path
import tempfile
import types
from unittest.mock import patch

from ocr_layout import (combine_regions, deduplicate_spotted_blocks, image_tiles,
                        normalize_box, offset_box, owns_spotted_box, rectangle_box, task_prompt)


class OcrStructureTests(unittest.TestCase):
    def test_long_image_crops_preserve_pixels_and_original_coordinates(self):
        from PIL import Image
        # A long document is not squeezed into a 2048px thumbnail.
        image = Image.new('RGB', (600, 6500), 'white')
        image.putpixel((317, 6401), (15, 37, 91))
        tiles = list(image_tiles(image))
        self.assertGreater(len(tiles), 1)
        self.assertEqual(image.size, (600, 6500))
        tile, bounds, _, _ = tiles[-1]
        self.assertEqual(tile.getpixel((317 - bounds[0], 6401 - bounds[1])), (15, 37, 91))
        self.assertTrue(all(t.width <= 2048 and t.height <= 2048 for t, *_ in tiles))
        self.assertEqual(bounds[3], 6500)
        near_limit = list(image_tiles(Image.new('RGB', (600, 2050))))
        self.assertEqual(near_limit[-1][0].height, 194)
        self.assertEqual(near_limit[-1][1][1], 1856)

    def test_native_prompt_contract_uses_supported_element_tasks(self):
        self.assertEqual(task_prompt('glm-ocr', 'code'), 'Text Recognition:')
        self.assertEqual(task_prompt('paddle-ocr-vl', 'document'), 'OCR:')
        self.assertEqual(task_prompt('paddle-ocr-vl', 'formula'), 'Formula Recognition:')
        self.assertIn('LaTeX', task_prompt('hunyuan-ocr', 'formula'))
        self.assertIn('缩进', task_prompt('hunyuan-ocr', 'code'))

    def test_code_keeps_indentation_underscores_and_repeated_lines(self):
        code = 'def total():\n    unit_price = 5\n    return unit_price\n'
        result = combine_regions([dict(text='```python\n' + code + '```', bounds=(0, 0, 100, 80))],
                                 'code', 'hunyuan-ocr', 100, 80)
        self.assertEqual(result['text'], code[:-1])
        self.assertEqual(result['blocks'][0]['text'], result['text'])
        self.assertEqual(result['blocks'][0]['box'], rectangle_box((0, 0, 100, 80)))
        self.assertTrue(result['blocks'][0]['region'])
        repeated = combine_regions([dict(text='    same_line()', bounds=(0, 0, 100, 80)),
                                    dict(text='    same_line()', bounds=(0, 60, 100, 140))],
                                   'code', 'hunyuan-ocr', 100, 140)
        self.assertEqual(repeated['text'], '    same_line()\n    same_line()')
        self.assertTrue(repeated['warning'])

    def test_formula_and_html_transport_do_not_rewrite_structure(self):
        formula = r'\frac{a_b}{\sqrt{x}} + \begin{matrix}1&2\\3&4\end{matrix}'
        result = combine_regions([dict(text=formula, bounds=(10, 20, 300, 150))],
                                 'formula', 'glm-ocr', 500, 500)
        self.assertEqual(result['text'], formula)
        self.assertEqual(result['format'], 'latex')
        table = '<table><tr><td rowspan="2">a_b</td><td></td></tr></table>'
        result = combine_regions([dict(text=table, bounds=(0, 0, 500, 300))],
                                 'table', 'paddle-ocr-vl', 500, 300)
        self.assertEqual(result['text'], table)
        self.assertEqual(result['format'], 'html')
        self.assertEqual(result['warning'], '')

    def test_text_overlap_dedup_and_raw_regions_are_both_available(self):
        result = combine_regions([dict(text='第一行\n第二行', bounds=(0, 0, 100, 80)),
                                  dict(text='第二行\n第三行', bounds=(0, 60, 100, 140))],
                                 'text', 'glm-ocr', 100, 140)
        self.assertEqual(result['text'], '第一行\n第二行\n第三行')
        self.assertEqual(result['raw']['regions'][1]['text'], '第二行\n第三行')
        self.assertEqual(result['blocks'][1]['box'][0], [0, 60])

    def test_provider_boxes_offset_and_only_overlap_duplicates_removed(self):
        box = normalize_box([{'X': 2, 'Y': 3}, {'X': 42, 'Y': 3},
                             {'X': 42, 'Y': 23}, {'X': 2, 'Y': 23}], 100, 100)
        self.assertEqual(offset_box(box, 10, 70, 200, 200), rectangle_box((12, 73, 52, 93)))
        self.assertEqual(normalize_box([[0, 0], [50, 0], [50, float('nan')]], 100, 100), [])
        self.assertEqual(normalize_box({'left': 1, 'top': 2, 'width': -1, 'height': 5}, 100, 100), [])
        blocks = [dict(text='同一行', box=rectangle_box((2, 3, 42, 23))),
                  dict(text='同一行', box=rectangle_box((3, 4, 43, 24))),
                  dict(text='同一行', box=rectangle_box((2, 80, 42, 100)))]
        self.assertEqual(len(deduplicate_spotted_blocks(blocks)), 2)

    def test_overlap_ownership_does_not_depend_on_recognized_spelling(self):
        # One physical line predicted differently by the two neighboring crops.
        first = rectangle_box((20, 1940, 300, 1980))
        second = rectangle_box((20, 84, 300, 124))
        self.assertFalse(owns_spotted_box(first, (0, 0, 500, 2048), 500, 6000))
        self.assertTrue(owns_spotted_box(second, (0, 1856, 500, 3904), 500, 6000))
        # First and final image boundaries do not discard valid source lines.
        self.assertTrue(owns_spotted_box(rectangle_box((2, 2, 100, 22)), (0, 0, 500, 2048), 500, 6000))
        self.assertTrue(owns_spotted_box(rectangle_box((2, 128, 100, 142)), (0, 5856, 500, 6000), 500, 6000))

    def test_truncation_is_explicit_and_too_many_tiles_are_rejected(self):
        from PIL import Image
        result = combine_regions([dict(text='未完成', bounds=(0, 0, 100, 80), truncated=True)],
                                 'text', 'glm-ocr', 100, 80)
        self.assertTrue(result['truncated'])
        self.assertIn('不完整', result['warning'])
        with self.assertRaisesRegex(ValueError, '64 个'):
            list(image_tiles(Image.new('RGB', (10, 150000))))

    def test_actual_vlm_adapter_processes_long_image_without_downloading_or_resizing(self):
        from PIL import Image
        import ocr_runtime
        calls = []
        class Tensor:
            shape = (1, 4)
            def __getitem__(self, _):
                return self
        class Inputs(dict):
            def to(self, _):
                return self
        class Processor:
            def apply_chat_template(self, messages, **_):
                content = messages[0]['content']
                self.prompt = content[1]['text']
                return Inputs(input_ids=Tensor(), image=content[0]['image'])
            def batch_decode(self, *_args, **_kwargs):
                return ['    unit_price = 5\n    return unit_price']
        processor = Processor()
        class Factory:
            @staticmethod
            def from_pretrained(_folder, **options):
                self.assertTrue(options['local_files_only'])
                self.assertFalse(options['trust_remote_code'])
                return processor
        class Model:
            def to(self, _):
                return self
            def eval(self):
                return self
            def generate(self, **options):
                calls.append(options['image'].size)
                return Tensor()
        class ModelFactory:
            @staticmethod
            def from_pretrained(_folder, **options):
                self.assertTrue(options['local_files_only'])
                self.assertFalse(options['trust_remote_code'])
                return Model()
        torch = types.SimpleNamespace(cuda=types.SimpleNamespace(is_available=lambda: False),
                                      float32='float32', bfloat16='bfloat16', inference_mode=contextlib.nullcontext)
        transformers = types.SimpleNamespace(AutoProcessor=Factory, AutoModelForImageTextToText=ModelFactory,
                                             StoppingCriteria=object, StoppingCriteriaList=list)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'glm-ocr').mkdir()
            (root / 'glm-ocr' / 'ready.json').write_text('{}')
            with patch.dict('sys.modules', torch=torch, transformers=transformers):
                recognize, device = ocr_runtime.load_vlm(root, 'glm-ocr')
                result = recognize(Image.new('RGB', (600, 6500)), 'code', 4096)
        self.assertEqual(device, 'cpu')
        self.assertEqual(processor.prompt, 'Text Recognition:')
        self.assertGreater(len(calls), 1)
        self.assertTrue(all(w == 600 and h <= 2048 for w, h in calls))
        self.assertEqual(len(result['blocks']), len(calls))
        self.assertEqual(result['blocks'][-1]['box'][2], [600, 6500])
        self.assertTrue(result['text'].startswith('    unit_price'))


if __name__ == '__main__':
    unittest.main()
