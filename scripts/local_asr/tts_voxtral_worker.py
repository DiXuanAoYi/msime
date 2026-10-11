# /// script
# requires-python = ">=3.12,<3.13"
# dependencies = ["numpy==2.3.5", "torch==2.9.1+cpu", "transformers==5.15.1", "safetensors==0.8.0", "tiktoken==0.12.0", "blobfile==3.1.0"]
# [tool.uv.sources]
# torch = { index = "pytorch" }
# [[tool.uv.index]]
# name = "pytorch"
# url = "https://download.pytorch.org/whl/cpu"
# explicit = true
# ///
"""Official local weights -> pinned audio.cpp Q8 conversion -> native CPU stream."""
from pathlib import Path
import sys
from tts_manager import serve
from tts_stream import StreamingSynthesizer
from tts_voxtral_native import VoxtralNative
from tts_voxtral_convert import prepare_model


def load(root, model_id):
    directory = root / model_id
    native = Path(__file__).parent / 'native-voxtral'
    model = prepare_model(directory, native)
    backend = VoxtralNative(native / 'audiocpp.dll', model)
    languages = {'en','fr','es','de','it','pt','nl','ar','hi'}
    def generate(text, voice, language, speed, options, emit, cancelled):
        if language not in languages:
            raise ValueError('Voxtral 不支持中文或中文方言，请选择英/法/西/德/意/葡/荷/阿/印地语')
        backend.generate(text, voice or 'casual_male', language, speed, options, emit, cancelled)
    # Keep the backend alive for the complete service lifetime.
    return StreamingSynthesizer(generate, 'audio.cpp · Q8_0 · Windows CPU 流式', accepts_options=True)


if __name__ == '__main__':
    serve(Path(sys.argv[1]), sys.argv[2], load, externally_locked=True)
