# /// script
# requires-python = ">=3.12,<3.13"
# dependencies = ["numpy==2.3.5", "onnxruntime-gpu[cuda,cudnn]==1.23.2", "sentencepiece==0.2.1"]
# ///
"""GPU-first MOSS adapter. No model downloads, explicit CPU fallback."""
from pathlib import Path
import sys
from tts_moss_worker import load as load_model
from tts_manager import serve

def load(root, model_id):
    return load_model(root, model_id, prefer_gpu=True)

if __name__ == '__main__':
    serve(Path(sys.argv[1]), sys.argv[2], load, externally_locked=True)
