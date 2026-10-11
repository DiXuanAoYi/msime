"""Validate model-specific controls using the same catalog shown by Settings."""
import json
import math


def moss_generation_options(defaults, options):
    generation = {**defaults, **{k:v for k,v in options.items() if k != 'chunk_frames'}}
    if not generation['do_sample']:
        generation['sample_mode'] = 'greedy'
    elif any(k.startswith('audio_') and value != defaults.get(k) for k, value in options.items()):
        generation['sample_mode'] = 'full'
    return generation


def validate_options(value, model):
    if not isinstance(value, dict) or len(value) > 24:
        raise ValueError('模型参数必须是有效对象')
    controls = {item['id']: item for item in model.get('controls', [])}
    result = {}
    for key, item in value.items():
        if key not in controls:
            raise ValueError('此模型不支持参数：' + key)
        rule = controls[key]
        kind = rule['type']
        if kind == 'bool':
            valid = isinstance(item, bool)
        elif kind in ('int', 'float'):
            valid = isinstance(item, (int, float)) and not isinstance(item, bool) and math.isfinite(item)
            valid = valid and rule['min'] <= item <= rule['max']
            if kind == 'int':
                valid = valid and int(item) == item
                if valid:
                    item = int(item)
        elif kind == 'enum':
            valid = isinstance(item, str) and item in rule['values']
        else:
            valid = isinstance(item, str) and len(item.encode('utf-8')) <= rule.get('maxBytes', 1000)
            valid = valid and '\0' not in item
        if not valid:
            raise ValueError('模型参数超出范围：' + key)
        result[key] = item
    return json.dumps(result, sort_keys=True, ensure_ascii=False, separators=(',', ':'))
