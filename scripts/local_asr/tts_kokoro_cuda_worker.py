# /// script
# requires-python = ">=3.12,<3.13"
# dependencies = ["numpy==2.3.5", "onnxruntime-gpu[cuda,cudnn]==1.23.2", "sherpa-onnx @ https://huggingface.co/csukuangfj2/sherpa-onnx-wheels/resolve/main/cuda/1.13.8/sherpa_onnx-1.13.8%2Bcuda12.cudnn9-cp312-cp312-win_amd64.whl"]
# ///
"""GPU-capable official sherpa wheel; use existing downloaded Kokoro weights."""
from pathlib import Path
import sys
if sys.platform=='win32':
    import ctypes
    ctypes.windll.kernel32.SetErrorMode(0x8003)
import onnxruntime as ort
from tts_manager import load_kokoro,serve
def load(root,model_id):
    try:
        ort.preload_dlls()
        model=load_kokoro(root,model_id,provider='cuda')
        # Exercise CUDA kernels before marking ready. Never play the warm-up.
        model.generate('Hello.','af_heart','en',1,{},lambda samples,rate:None,lambda:False)
        return model
    except Exception as error:
        print('Kokoro CUDA unavailable; CPU fallback:',str(error)[:250],file=sys.stderr)
        model=load_kokoro(root,model_id,provider='cpu')
        model.device='Kokoro CPU · CUDA 初始化或算子不支持后回退'
        return model
if __name__=='__main__':serve(Path(sys.argv[1]),sys.argv[2],load,externally_locked=True)
