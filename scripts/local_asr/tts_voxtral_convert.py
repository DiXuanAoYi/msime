"""Windows adaptation of audio.cpp's pinned Voxtral converter.

Source: audio.cpp 96269554d00621fb0fca6b52b58919749eba1c8d,
tests/voxtral_tts/convert_gguf.py (Apache-2.0).
Differences: user-owned staging, hardlink instead of privileged symlink,
atomic completion marker, and explicit native runtime paths.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


VERSION = 'audio.cpp-96269554-q8-v1'


def convert_tokenizer(source, staging):
    from transformers.integrations.tiktoken import TikTokenConverter
    tekken = json.loads((source / 'tekken.json').read_text(encoding='utf-8'))
    config = tekken['config']
    offset = config['default_num_special_tokens']
    count = config['default_vocab_size'] - offset
    ranks = staging / 'text.tiktoken'
    ranks.write_text(''.join(f"{token['token_bytes']} {token['rank']}\n" for token in tekken['vocab'][:count]), encoding='utf-8')
    tokenizer = TikTokenConverter(vocab_file=str(ranks), pattern=config['pattern']).converted()
    exported = json.loads(tokenizer.to_str())
    exported['model']['vocab'] = {token: rank + offset for token, rank in exported['model']['vocab'].items()}
    specials = {token['rank']: token['token_str'] for token in tekken['special_tokens']}
    exported['added_tokens'] = [dict(id=rank, content=specials.get(rank, f'<SPECIAL_{rank}>'), single_word=False,
                                    lstrip=False, rstrip=False, normalized=False, special=True) for rank in range(offset)]
    (staging / 'tokenizer.json').write_text(json.dumps(exported, ensure_ascii=False), encoding='utf-8')
    (staging / 'tokenizer_config.json').write_text(json.dumps(dict(bos_token='<s>',eos_token='</s>',pad_token='<pad>')), encoding='utf-8')
    ranks.unlink()


def prepare_model(directory, native):
    from manager import atomic_json
    output = directory / 'converted' / 'voxtral-q8.gguf'
    marker = output.with_suffix('.json')
    ready_path = directory / 'ready.json'
    if not ready_path.is_file():
        raise ValueError('请先下载并校验完整的 Voxtral 官方权重')
    identity = dict(version=VERSION, source=json.loads(ready_path.read_text(encoding='utf-8')))
    try:
        previous = json.loads(marker.read_text(encoding='utf-8'))
        if previous.get('identity') == identity and previous.get('bytes') == output.stat().st_size and output.stat().st_size > 0:
            return output
    except (OSError, ValueError):
        pass
    converter = native / 'audiocpp_gguf.exe'
    if not converter.is_file():
        raise RuntimeError('Voxtral 本机转换组件缺失，请更新完整安装包')
    source = directory / 'original'
    import torch
    from safetensors import safe_open
    from safetensors.torch import save_file
    output.parent.mkdir(parents=True, exist_ok=True)
    atomic_json(directory.parent / 'service.json', dict(state='loading', model='voxtral-tts', pid=os.getpid(),
                message='正在本机转换 Voxtral Q8 权重（只处理已下载文件，首次较慢）'))
    # The conversion's temporary files stay inside this model directory.
    with tempfile.TemporaryDirectory(prefix='convert-', dir=directory) as temporary:
        staging = Path(temporary)
        (staging / 'params.json').write_bytes((source / 'params.json').read_bytes())
        convert_tokenizer(source, staging)
        shard = 'consolidated.safetensors'
        try:
            os.link(source / shard, staging / shard)
        except OSError:
            shutil.copyfile(source / shard, staging / shard)
        derived = {}
        with safe_open(source / shard, framework='pt', device='cpu') as tensors:
            weight_map = {name: shard for name in tensors.keys()}
            for name in list(weight_map):
                if name.endswith('.parametrizations.weight.original1'):
                    prefix = name.removesuffix('.parametrizations.weight.original1')
                    magnitude = prefix + '.parametrizations.weight.original0'
                    derived[prefix + '.weight'] = torch._weight_norm(tensors.get_tensor(name), tensors.get_tensor(magnitude), 0).contiguous()
                    del weight_map[name], weight_map[magnitude]
            quantizer = 'audio_tokenizer.quantizer.semantic_codebook.'
            derived[quantizer+'embedding'] = (tensors.get_tensor(quantizer+'embedding_sum') / tensors.get_tensor(quantizer+'cluster_usage').clamp(min=1e-5)[:,None]).contiguous()
            del weight_map[quantizer+'cluster_usage'], weight_map[quantizer+'embedding_sum']
        voices = json.loads((source/'params.json').read_text(encoding='utf-8'))['multimodal']['audio_tokenizer_args']['voice']
        for voice in voices:
            # Voice names come from the hash-verified official params, not UI paths.
            if not isinstance(voice,str) or '/' in voice or '\\' in voice or '..' in voice:
                raise ValueError('Voxtral 音色清单无效')
            prompt = torch.load(source/'voice_embedding'/f'{voice}.pt', weights_only=True, map_location='cpu')
            if prompt.ndim != 2:
                raise ValueError('Voxtral 音色 embedding 无效')
            derived[f'voices.{voice}.prompt_embedding'] = prompt.contiguous()
        save_file(derived, staging/'derived.safetensors')
        weight_map.update({name:'derived.safetensors' for name in derived})
        (staging/'model.safetensors.index.json').write_text(json.dumps(dict(weight_map=weight_map)),encoding='utf-8')
        temp_output = output.with_suffix('.part.gguf')
        command = [str(converter),'--input',str(staging/'model.safetensors.index.json'),'--root',str(staging),
                   '--output',str(temp_output),'--type','q8_0','--family','voxtral_tts','--model-spec',
                   str(Path(__file__).parent/'vendor/voxtral/model_spec.json'),'--keep-type','voices.*=bf16','--overwrite']
        subprocess.run(command, check=True, creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        subprocess.run([str(converter),'--inspect',str(temp_output)], check=True, creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        os.replace(temp_output, output)
        atomic_json(marker, dict(identity=identity,bytes=output.stat().st_size))
    return output
