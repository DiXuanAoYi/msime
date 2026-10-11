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
from tts_manager import serve
from tts_stream import StreamingSynthesizer

def load(root, model_id):
    torch.set_num_threads(4)
    model = Qwen3TTSModel.from_pretrained(str(root / model_id / 'original'),
        dtype=torch.bfloat16 if torch.cuda.is_available() else torch.float32,
        device_map='cuda:0' if torch.cuda.is_available() else 'cpu', attn_implementation='sdpa', local_files_only=True)
    languages={'zh':'Chinese','en':'English','ja':'Japanese','ko':'Korean','de':'German','fr':'French','ru':'Russian','es':'Spanish','it':'Italian','pt':'Portuguese'}
    def synthesize(text, voice, language, speed, options, emit, cancelled):
        if language not in languages:
            raise ValueError('此模型不支持所选语言')
        codes = []
        emitted = 0
        def flush():
            nonlocal emitted
            if len(codes) <= emitted:
                return
            # Decode a bounded left context; Qwen's 12 Hz tokenizer emits 2000
            # samples per codec frame at 24 kHz. Preserve 25 prior frames.
            start = max(0, emitted - 25)
            audio, rate = model.model.speech_tokenizer.decode({'audio_codes': torch.stack(codes[start:], dim=1)})
            samples = np.asarray(audio[0]).reshape(-1)[(emitted - start) * 2000:]
            emitted = len(codes)
            if speed != 1:
                samples = np.interp(np.arange(0, len(samples), speed), np.arange(len(samples)), samples)
            if len(samples):
                emit(samples, rate)
        def on_forward(module, inputs, output):
            if cancelled():
                raise InterruptedError('已停止朗读')
            codec = output.hidden_states[-1]
            if codec is not None and int(codec[0, 0]) < module.config.vocab_size - 1024:
                codes.append(codec.detach())
                if len(codes) - emitted >= options.get('chunk_size', 6):
                    flush()
        handle = model.model.talker.register_forward_hook(on_forward)
        try:
            kwargs = {'max_new_tokens': 1024, **{k:v for k,v in options.items() if k != 'chunk_size'}}
            model.generate_custom_voice(text=text, language=languages[language], speaker=voice or 'Vivian', **kwargs)
            flush()
        finally:
            handle.remove()
    return StreamingSynthesizer(synthesize, 'CPU · Qwen 流式', accepts_options=True)

if __name__ == '__main__':
    serve(Path(sys.argv[1]),sys.argv[2],load,externally_locked=True)
