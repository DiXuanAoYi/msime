# /// script
# requires-python = ">=3.12,<3.13"
# dependencies = ["qwen-tts==0.1.1", "torch==2.9.1", "torchaudio==2.9.1", "numpy==2.3.5"]
# ///
"""Official Qwen CustomVoice adapter in an isolated runtime."""
from pathlib import Path
import sys
from worker_dependencies import validate_torch_audio
validate_torch_audio()
import numpy as np
import torch
from qwen_tts import Qwen3TTSModel
from tts_manager import serve, pcm_wav

def load(root, model_id):
    model = Qwen3TTSModel.from_pretrained(str(root / model_id / 'original'),
        dtype=torch.bfloat16 if torch.cuda.is_available() else torch.float32,
        device_map='cuda:0' if torch.cuda.is_available() else 'cpu', attn_implementation='sdpa', local_files_only=True)
    languages={'zh':'Chinese','en':'English','ja':'Japanese','ko':'Korean','de':'German','fr':'French','ru':'Russian','es':'Spanish','it':'Italian','pt':'Portuguese'}
    def synthesize(text, voice, language, speed):
        if language not in languages:
            raise ValueError('此模型不支持所选语言')
        audios, rate = model.generate_custom_voice(text=text,language=languages[language],speaker=voice or 'Vivian', max_new_tokens=2048)
        samples=np.asarray(audios[0])
        # Apply speed locally without requesting cloud processing.
        if speed != 1:
            samples=np.interp(np.arange(0,len(samples),speed),np.arange(len(samples)),samples)
        return pcm_wav(samples,rate)
    return synthesize

serve(Path(sys.argv[1]),sys.argv[2],load,externally_locked=True)
