# /// script
# requires-python = ">=3.12,<3.13"
# dependencies = ["edge-tts==7.2.8", "av==19.0.1", "numpy==2.3.5"]
# ///
"""Explicit opt-in ONLINE adapter. Decode MP3 packets as the service streams them."""
import asyncio
from pathlib import Path
import sys
import av
import edge_tts
from tts_manager import serve
from tts_stream import StreamingSynthesizer

def load(root, model_id):
    def generate(text, voice, language, speed, emit, cancelled):
        async def synthesize():
            codec = av.CodecContext.create('mp3float', 'r')
            voice_id = voice or ('zh-CN-XiaoxiaoNeural' if language == 'zh' else 'en-US-JennyNeural')
            stream = edge_tts.Communicate(text, voice_id, rate=f'{int((speed - 1) * 100):+d}%')
            async for item in stream.stream():
                if cancelled():
                    raise InterruptedError('已停止朗读')
                if item['type'] != 'audio':
                    continue
                for packet in codec.parse(item['data']):
                    for frame in codec.decode(packet):
                        emit(frame.to_ndarray().T, frame.sample_rate)
            for packet in codec.parse(b''):
                for frame in codec.decode(packet):
                    emit(frame.to_ndarray().T, frame.sample_rate)
            for frame in codec.decode(None):
                emit(frame.to_ndarray().T, frame.sample_rate)
        asyncio.run(synthesize())
    return StreamingSynthesizer(generate, 'Edge-TTS 在线服务 · 文本将发送到微软')

if __name__ == '__main__':
    serve(Path(sys.argv[1]), sys.argv[2], load, externally_locked=True)
