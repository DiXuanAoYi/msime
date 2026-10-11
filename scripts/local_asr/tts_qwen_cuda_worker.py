# /// script
# requires-python = ">=3.12,<3.13"
# dependencies = ["faster-qwen3-tts==0.5.4", "qwen-tts-hf==0.1.1.post1", "transformers==5.15.1", "torch==2.9.1+cu128", "torchaudio==2.9.1+cu128", "numpy==2.3.5"]
# [tool.uv.sources]
# torch = { index = "pytorch" }
# torchaudio = { index = "pytorch" }
# [[tool.uv.index]]
# name = "pytorch"
# url = "https://download.pytorch.org/whl/cu128"
# explicit = true
# ///
"""CUDA graphs + incremental Qwen codec generation. Existing local weights only."""
from pathlib import Path
import sys
from worker_dependencies import validate_torch_audio
validate_torch_audio()
import numpy as np
import torch
from faster_qwen3_tts import FasterQwen3TTS
from tts_stream import StreamingSynthesizer
from tts_manager import serve

LANGUAGES = {'zh':'Chinese','en':'English','ja':'Japanese','ko':'Korean','de':'German','fr':'French','ru':'Russian','es':'Spanish','it':'Italian','pt':'Portuguese'}

def load(root, model_id):
    if not torch.cuda.is_available():
        raise RuntimeError('CUDA 不可用，请检查 NVIDIA 驱动和运行依赖；没有静默切换为 CPU')
    torch.set_num_threads(2)
    model = FasterQwen3TTS.from_pretrained(str(root / model_id / 'original'), device='cuda:0', dtype=torch.bfloat16, attn_implementation='sdpa', max_seq_len=2048, local_files_only=True)
    model.warmup(prefill_len=64)
    # Prime codec kernels before publishing running; no sound or spoken-text cache.
    for _ in model.generate_custom_voice_streaming(text='你好', speaker='Vivian', language='Chinese', max_new_tokens=6, chunk_size=6):
        pass
    def generate(text, voice, language, speed, emit, cancelled):
        if language not in LANGUAGES:
            raise ValueError('此模型不支持所选语言')
        with torch.inference_mode():
            for audio, rate, timing in model.generate_custom_voice_streaming(text=text, speaker=voice or 'Vivian', language=LANGUAGES[language], chunk_size=6, max_new_tokens=1024):
                if cancelled():
                    raise InterruptedError('已停止朗读')
                samples = np.asarray(audio).reshape(-1)
                if speed != 1:
                    samples = np.interp(np.arange(0, len(samples), speed), np.arange(len(samples)), samples)
                if len(samples):
                    emit(samples, rate)
    return StreamingSynthesizer(generate, torch.cuda.get_device_name(0) + ' · CUDA 流式')

if __name__ == '__main__':
    serve(Path(sys.argv[1]), sys.argv[2], load, externally_locked=True)
