# /// script
# requires-python = ">=3.12,<3.13"
# dependencies = ["qwen-asr==0.0.6", "torch==2.9.1", "torchaudio==2.9.1", "numpy==2.3.5"]
# ///
"""Official Qwen ASR runtime, isolated from the IME and the system Python."""
from pathlib import Path
import sys
from worker_dependencies import validate_torch_audio
validate_torch_audio()
import numpy as np
import torch
from qwen_asr import Qwen3ASRModel
from manager import serve


def load(root, model_id):
    model = Qwen3ASRModel.from_pretrained(
        str(root / model_id / "original"),
        dtype=torch.bfloat16 if torch.cuda.is_available() else torch.float32,
        device_map="cuda:0" if torch.cuda.is_available() else "cpu",
        max_inference_batch_size=1, max_new_tokens=512,
        local_files_only=True,
    )
    def transcribe(samples):
        return model.transcribe(audio=(np.asarray(samples, dtype=np.float32), 16000), language=None)[0].text
    return transcribe


serve(Path(sys.argv[1]), sys.argv[2], load, externally_locked=True)
