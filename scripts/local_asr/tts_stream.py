"""Model-neutral callback -> PCM transport. No disk cache of spoken content."""
from collections import OrderedDict
import io
import json
from pathlib import Path
import struct
import time
import wave
import numpy as np

_here = Path(__file__).resolve().parent
_contract = _here / 'tts_stream.json'
if not _contract.exists():
    _contract = _here.parents[1] / 'engine/contracts/tts_stream.json'
CONTRACT = json.loads(_contract.read_text(encoding='utf-8'))

class StreamingSynthesizer:
    """generate(text, voice, language, speed, emit, cancelled) emits during inference.

    emit(samples, sample_rate) is synchronous: transport backpressure bounds memory.
    Model adapters must poll cancelled while generating, including before first PCM.
    Only successfully completed output enters this process-local bounded cache.
    """
    def __init__(self, generate, device='CPU', accepts_options=False):
        self.generate = generate
        self.device = device
        self.accepts_options = accepts_options
        self.cache = OrderedDict()
        self.cache_bytes = 0

    def stream(self, args, write, cancelled=lambda: False):
        header = None
        total = 0
        saved = []
        started = time.perf_counter()
        first = None
        def emit(samples, rate):
            nonlocal header, total, first
            if cancelled():
                raise InterruptedError('已停止朗读')
            samples = np.asarray(samples)
            if samples.ndim == 1:
                samples = samples[:, None]
            if samples.ndim != 2 or not len(samples) or not np.isfinite(samples).all():
                raise ValueError('音频数据无效')
            channels = samples.shape[1]
            if not CONTRACT['min_rate'] <= rate <= CONTRACT['max_rate'] or not 1 <= channels <= CONTRACT['max_channels']:
                raise ValueError('不支持的采样率或声道')
            current = (int(rate), channels)
            if header is None:
                header = current
                write(CONTRACT['magic'].encode('ascii') + struct.pack('<II', *header))
            elif current != header:
                raise ValueError('流中采样格式发生变化')
            pcm = (np.clip(samples, -1, 1) * 32767).astype('<i2').tobytes()
            total += len(pcm)
            if total > rate * channels * 2 * CONTRACT['max_audio_seconds']:
                raise ValueError('音频超过 90 秒，请缩短文字')
            for offset in range(0, len(pcm), CONTRACT['max_chunk_bytes']):
                if cancelled():
                    raise InterruptedError('已停止朗读')
                chunk = pcm[offset:offset + CONTRACT['max_chunk_bytes']]
                frame = struct.pack('<I', len(chunk)) + chunk
                write(frame)
                saved.append(frame)
                if first is None:
                    first = time.perf_counter() - started
        key = tuple(args)
        use_cache = not (len(args) == 5 and json.loads(args[4]).get('seed') == -1)
        if use_cache and key in self.cache:
            self.cache.move_to_end(key)
            for index, frame in enumerate(self.cache[key]):
                if cancelled():
                    raise InterruptedError('已停止朗读')
                write(frame)
                if index == 1:
                    first = time.perf_counter() - started
            return {'cached': True, 'first_chunk_ms': first * 1000, 'generation_ms': 0}
        if self.accepts_options:
            options = json.loads(args[4]) if len(args) == 5 else {}
            self.generate(*args[:4], options, emit, cancelled)
        else:
            if len(args) != 4:
                raise ValueError('此适配器不支持额外参数')
            self.generate(*args, emit, cancelled)
        if header is None or cancelled():
            raise InterruptedError('没有生成音频或已取消')
        write(struct.pack('<I', 0))
        frames = [CONTRACT['magic'].encode('ascii') + struct.pack('<II', *header), *saved, struct.pack('<I', 0)]
        size = sum(map(len, frames))
        budget = 16 * 1024 * 1024
        if use_cache and size <= budget:
            while self.cache and (self.cache_bytes + size > budget or len(self.cache) >= 128):
                _, old = self.cache.popitem(last=False)
                self.cache_bytes -= sum(map(len, old))
            self.cache[key] = frames
            self.cache_bytes += size
        return {'cached': False, 'first_chunk_ms': first * 1000, 'generation_ms': (time.perf_counter() - started) * 1000}

    def __call__(self, *args):
        # Compatibility for external clients that explicitly request full WAV.
        frames = []
        self.stream(args, frames.append)
        body = b''.join(frames)
        rate, channels = struct.unpack_from('<II', body, 8)
        offset, pcm = 16, bytearray()
        while True:
            length, = struct.unpack_from('<I', body, offset)
            offset += 4
            if not length:
                break
            pcm.extend(body[offset:offset + length])
            offset += length
        output = io.BytesIO()
        with wave.open(output, 'wb') as wav:
            wav.setparams((channels, 2, rate, 0, 'NONE', 'not compressed'))
            wav.writeframes(pcm)
        return output.getvalue()
