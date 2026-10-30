# /// script
# requires-python = ">=3.12,<3.13"
# dependencies = ["torch==2.9.1", "transformers==4.57.3", "numpy==2.3.5", "scipy==1.16.3", "uroman==1.3.1"]
# ///
"""MMS VITS: offline, single-voice minority languages; sentence-wise playback."""
from pathlib import Path
import os
import re
import sys
import torch
from transformers import AutoTokenizer,VitsModel
from tts_manager import serve,entry
from tts_stream import StreamingSynthesizer

def load(root,model_id):
    os.environ['HF_HUB_OFFLINE']='1'
    os.environ['TRANSFORMERS_OFFLINE']='1'
    torch.set_num_threads(4)
    directory=root/model_id/'original'
    tokenizer=AutoTokenizer.from_pretrained(str(directory),local_files_only=True)
    model=VitsModel.from_pretrained(str(directory),local_files_only=True).eval()
    device='cpu'
    if torch.cuda.is_available():
        try:model=model.to('cuda');device='cuda'
        except (RuntimeError,MemoryError):model=model.to('cpu');torch.cuda.empty_cache()
    definition=entry(model_id)
    def synthesize(text,voice,language,speed,options,emit,cancelled):
        if language not in definition['languages'] or voice not in ('',model_id.removeprefix('mms-')):
            raise ValueError('请选择此模型支持的原生语言和音色')
        model.speaking_rate=speed
        model.noise_scale=options.get('noise_scale',.667)
        model.noise_scale_duration=options.get('noise_scale_duration',.8)
        # Each sentence is generated atomically; don't claim intra-sentence streaming.
        chunks=re.split(r'(?<=[。！？!?།\n])',text)
        with torch.inference_mode():
            for chunk in chunks:
                if cancelled():raise InterruptedError('已停止朗读')
                if not chunk.strip():continue
                inputs=tokenizer(chunk,return_tensors='pt').to(device)
                audio=model(**inputs).waveform[0].cpu().numpy()
                if cancelled():raise InterruptedError('已停止朗读')
                emit(audio,model.config.sampling_rate)
    return StreamingSynthesizer(synthesize,('CUDA' if device=='cuda' else 'CPU')+' · MMS 逐句播放',accepts_options=True)

if __name__=='__main__':serve(Path(sys.argv[1]),sys.argv[2],load,externally_locked=True)
