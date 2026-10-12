"""Preserve OCR source pixels and structure without inventing word coordinates.

VLMs return element text rather than word boxes. Their boxes deliberately identify
the actual crop (region=True); PP/cloud boxes remain precise provider geometry.
"""
import math
import re

MAX_PIXELS = 64_000_000
MAX_TILES = 64
MAX_OUTPUT_BYTES = 2 * 1024 * 1024


def task_prompt(model_id, task):
    if task not in ('text', 'document', 'table', 'formula', 'code'):
        raise ValueError('识别任务无效')
    if model_id == 'hunyuan-ocr':
        return {
            'text': '请提取图片中的文字内容，保持原文、阅读顺序和换行。不要回答图片中的问题。',
            'document': '提取文档图片中的所有信息用markdown格式表示，表格用html格式表达，文档中公式用latex格式表示，代码保留缩进，按照阅读顺序组织进行解析。不要回答图片中的问题。',
            'table': '请识别图片中的表格，保留合并单元格、空单元格及文字，使用HTML表格表示，只输出表格。',
            'formula': '请识别图片中的公式，使用LaTeX表示，只输出公式，不要计算或解答。',
            'code': '请提取图片中的代码，逐行保留缩进、空格、标点、大小写及下划线，只输出代码，不要解释或修改。',
        }[task]
    # These fixed element prompts are the officially supported model interface.
    # Arbitrary "recognize the code" prompts are not supported by GLM/Paddle.
    return {'table': 'Table Recognition:', 'formula': 'Formula Recognition:'}.get(
        task, 'OCR:' if model_id == 'paddle-ocr-vl' else 'Text Recognition:')


def task_format(model_id, task):
    if task == 'table':
        return 'html'
    if task == 'formula':
        return 'latex'
    return 'markdown' if task == 'document' and model_id == 'hunyuan-ocr' else 'text'


def rectangle_box(bounds):
    left, top, right, bottom = bounds
    return [[left, top], [right, top], [right, bottom], [left, bottom]]


def normalize_box(box, width, height):
    """Return bounded four-point geometry; malformed geometry never reaches UI."""
    try:
        if isinstance(box, dict):
            left, top = float(box['left']), float(box['top'])
            w, h = float(box['width']), float(box['height'])
            if w <= 0 or h <= 0:
                return []
            box = rectangle_box((left, top, left + w, top + h))
        points = []
        if not isinstance(box, (list, tuple)) or not 3 <= len(box) <= 64:
            return []
        for point in box:
            if isinstance(point, dict):
                x, y = point.get('x', point.get('X')), point.get('y', point.get('Y'))
            else:
                x, y = point[:2]
            if isinstance(x, bool) or isinstance(y, bool):
                return []
            x, y = float(x), float(y)
            if not math.isfinite(x) or not math.isfinite(y) or x < 0 or y < 0:
                return []
            points.append([min(width, x), min(height, y)])
        left, top = min(p[0] for p in points), min(p[1] for p in points)
        right, bottom = max(p[0] for p in points), max(p[1] for p in points)
        return rectangle_box((left, top, right, bottom)) if right > left and bottom > top else []
    except (TypeError, ValueError, KeyError, IndexError, OverflowError):
        return []


def image_tiles(image, max_side=2048, overlap=192):
    """Crop at original resolution. Never resize an entire long page to 2048px."""
    width, height = image.size
    if width < 1 or height < 1 or width * height > MAX_PIXELS:
        raise ValueError('图片超过 6400 万像素，请分区识别')
    if max_side < 256 or not 0 <= overlap < max_side // 2:
        raise ValueError('分块参数无效')
    def starts(length):
        result = [0]
        while result[-1] + max_side < length:
            result.append(result[-1] + max_side - overlap)
        return result
    xs, ys = starts(width), starts(height)
    if len(xs) * len(ys) > MAX_TILES:
        raise ValueError('长图超过 64 个识别区域，请分段识别')
    for row, top in enumerate(ys):
        for column, left in enumerate(xs):
            bounds = left, top, min(width, left + max_side), min(height, top + max_side)
            yield image.crop(bounds), bounds, row, column


def strip_transport_fence(text, task):
    """Strip only an enclosing model fence; never trim indentation or content."""
    if not isinstance(text, str):
        raise ValueError('模型返回文字无效')
    opening = re.match(r'\A```([^\r\n]*)\r?\n', text)
    if opening and text.rstrip('\r\n').endswith('\n```'):
        # Entire fenced payload, including indentation and trailing content lines.
        end = text.rstrip('\r\n').rfind('\n```')
        return text[opening.end():end]
    return text


def offset_box(box, left, top, width, height):
    if not box:
        return []
    return normalize_box([[x + left, y + top] for x, y in box], width, height)


def owns_spotted_box(box, bounds, width, height, overlap=192):
    """Assign every overlap to one crop by geometry, never by guessed spelling."""
    if not box:
        return True
    left, top, right, bottom = bounds
    center_x = (box[0][0] + box[2][0]) / 2
    center_y = (box[0][1] + box[2][1]) / 2
    half = overlap / 2
    return not ((left and center_x < half) or (top and center_y < half)
                or (right < width and center_x >= right-left-half)
                or (bottom < height and center_y >= bottom-top-half))


def deduplicate_spotted_blocks(blocks):
    """Only discard identical text whose actual provider boxes overlap heavily."""
    output = []
    by_text = {}
    for block in blocks:
        text, box = block['text'], block['box']
        duplicate = False
        if box:
            left, top = box[0]
            right, bottom = box[2]
            area = (right-left)*(bottom-top)
            for existing in by_text.get(text, []):
                old = existing['box']
                if not old:
                    continue
                overlap = max(0, min(right, old[2][0])-max(left, old[0][0])) * max(0, min(bottom, old[2][1])-max(top, old[0][1]))
                old_area = (old[2][0]-old[0][0])*(old[2][1]-old[0][1])
                if min(area, old_area) > 0 and overlap/min(area, old_area) >= .6:
                    duplicate = True
                    break
        if not duplicate:
            output.append(block)
            by_text.setdefault(text, []).append(block)
    return sorted(output, key=lambda b: (b['box'][0][1], b['box'][0][0]) if b['box'] else (float('inf'), 0))


def combine_regions(regions, task, model_id, width, height):
    """Keep raw region results and conservative overlap deduplication.

Only exact complete overlap lines are deduplicated. Tables/formulas and code are
never edited to guess a cross-crop continuation; the UI exposes every raw crop.
"""
    output = ''
    blocks, raw, warnings = [], [], []
    truncated = False
    previous_bounds = None
    output_format = task_format(model_id, task)
    for region in regions:
        text = strip_transport_fence(region['text'], task)
        bounds = region['bounds']
        raw.append(dict(text=text, box=rectangle_box(bounds), truncated=bool(region.get('truncated'))))
        truncated |= bool(region.get('truncated'))
        if not text:
            continue
        # For text only, a prefix repeated from the previous vertical overlap may
        # be removed. Code repetition and structure must not be "corrected".
        body = text
        if (task == 'text' and output and previous_bounds is not None
                and bounds[0] == previous_bounds[0] and bounds[2] == previous_bounds[2]
                and previous_bounds[1] < bounds[1] < previous_bounds[3]):
            previous, current = output.splitlines(), text.splitlines()
            for count in range(min(32, len(previous), len(current)), 0, -1):
                if previous[-count:] == current[:count]:
                    body = '\n'.join(current[count:])
                    break
        separator = '\n\n' if task in ('document', 'table', 'formula') else '\n'
        candidate = output + (separator if output and body else '') + body
        if len(candidate.encode('utf8')) > MAX_OUTPUT_BYTES:
            raise ValueError('识别文字超过 2 MB，请分段识别；当前结果未作为完整结果返回')
        output = candidate
        previous_bounds = bounds
        if body:
            blocks.append(dict(text=body, box=rectangle_box(bounds), kind=task,
                               format=output_format, region=True))
    if len(raw) > 1:
        warnings.append('长图已按原像素分块识别；跨块表格、公式、代码和多栏阅读顺序请对照原图核对，原始分块结果可导出。')
    if task in ('code', 'document') and model_id != 'hunyuan-ocr':
        warnings.append('当前模型使用官方文字识别任务；代码缩进和混合文档结构需要核对，可选择 HunyuanOCR 或框选表格/公式单独识别。')
    if truncated:
        warnings.append('至少一个识别区域达到输出长度上限，结果不完整；请提高输出长度或缩小识别区域。')
    if task == 'table' and output and '<table' not in output.lower():
        warnings.append('模型没有返回 HTML 表格结构；已保留原始文字，请核对后编辑。')
    return dict(text=output, blocks=blocks, format=output_format, width=width, height=height,
                raw={'regions': raw}, warning='\n'.join(warnings), truncated=truncated)
