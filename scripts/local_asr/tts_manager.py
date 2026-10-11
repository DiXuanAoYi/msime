"""Explicitly invoked local TTS. Reuses the pinned, resumable ASR downloader."""
from __future__ import annotations
import argparse
from collections import OrderedDict
import contextlib
import http.server
import io
import json
import math
import os
from pathlib import Path
import secrets
import sys
import wave
import manager as support

CATALOG = json.loads((support.BASE / "tts_catalog.json").read_text(encoding="utf-8"))

def entry(model_id):
    return next(x for x in CATALOG if x["id"] == model_id)

def validate_request(body, model_id):
    request = json.loads(body)
    text = request.get("input")
    if not isinstance(text, str) or not text.strip() or len(text.encode("utf-8")) > 2000:
        raise ValueError("朗读文字必须为 1–2000 UTF-8 字节")
    if request.get("model") != model_id or request.get("response_format", "wav") != "wav":
        raise ValueError("请选择正在运行的模型和 WAV 输出")
    speed = float(request.get("speed", 1))
    if not math.isfinite(speed) or not 0.5 <= speed <= 2:
        raise ValueError("语速必须在 0.5–2 之间")
    return text, str(request.get("voice", "")), str(request.get("language", "zh")), speed

def pcm_wav(samples, sample_rate):
    import numpy as np
    samples = np.asarray(samples)
    if samples.ndim != 1 or not len(samples) or len(samples) > sample_rate * 90:
        raise ValueError("音频为空或超过 90 秒，请缩短文字")
    result = io.BytesIO()
    with wave.open(result, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes((np.clip(samples, -1, 1) * 32767).astype("<i2").tobytes())
    return result.getvalue()

def cache_synthesis(synthesize, max_bytes=16 * 1024 * 1024, max_entries=128):
    """Bounded process-local LRU; never persist spoken text or audio to disk.

    The HTTP server handles one request at a time. A new loader/service creates
    a new cache, so changing the model cannot reuse another model's audio.
    """
    cache = OrderedDict()
    used_bytes = 0
    def cached(text, voice, language, speed):
        nonlocal used_bytes
        key = (text, voice, language, speed)
        if key in cache:
            cache.move_to_end(key)
            return cache[key]
        audio = synthesize(text, voice, language, speed)
        if max_entries > 0 and len(audio) <= max_bytes:
            while cache and (used_bytes + len(audio) > max_bytes or len(cache) >= max_entries):
                _, previous = cache.popitem(last=False)
                used_bytes -= len(previous)
            cache[key] = audio
            used_bytes += len(audio)
        return audio
    return cached

def load_kokoro(root, model_id):
    import sherpa_onnx
    directory = root / model_id / "active"
    config = sherpa_onnx.OfflineTtsConfig(
        model=sherpa_onnx.OfflineTtsModelConfig(
            kokoro=sherpa_onnx.OfflineTtsKokoroModelConfig(
                model=str(directory / "model.int8.onnx"), voices=str(directory / "voices.bin"),
                tokens=str(directory / "tokens.txt"), data_dir=str(directory / "espeak-ng-data"),
                dict_dir=str(directory / "dict"),
                lexicon=','.join(str(directory / n) for n in ['lexicon-zh.txt','lexicon-us-en.txt'])),
            num_threads=min(4, os.cpu_count() or 1), provider="cpu"),
        rule_fsts=','.join(str(directory / n) for n in ['phone-zh.fst','date-zh.fst','number-zh.fst']),
        max_num_sentences=1)
    if not config.validate():
        raise ValueError("Kokoro 运行包不完整")
    model = sherpa_onnx.OfflineTts(config)
    # v1.1's first 53 IDs retain v1.0's named voices.
    voices = {"af_heart":3, "zf_xiaobei":45, "zf_xiaoni":46, "zm_yunjian":49}
    def synthesize(text, voice, language, speed):
        if language not in ('zh', 'en'):
            raise ValueError("此 Kokoro 运行包支持中文和英文")
        sid = int(voice) if voice.isdecimal() else voices.get(voice, -1)
        if sid < 0 or sid >= model.num_speakers:
            raise ValueError("音色无效，请使用 af_heart / zf_xiaobei 或有效数字音色 ID")
        audio = model.generate(text, sid=sid, speed=speed)
        return pcm_wav(audio.samples, audio.sample_rate)
    return synthesize

def serve(root, model_id, loader=load_kokoro, externally_locked=False):
    state = root / "service.json"
    stop = root / "stop"
    with contextlib.nullcontext() if externally_locked else support.job_lock(root / "service.lock"):
        if not externally_locked:
            stop.unlink(missing_ok=True)
        token = secrets.token_hex(32)
        (root / "service-token.txt").write_text(token, encoding="ascii")
        support.atomic_json(state, dict(state="loading", model=model_id, message="正在加载发音模型", pid=os.getpid()))
        try:
            synthesize = cache_synthesis(loader(root, model_id))
            class Handler(http.server.BaseHTTPRequestHandler):
                timeout = 15
                def log_message(self, *args):
                    pass
                def reply(self, code, data, kind="application/json"):
                    self.send_response(code)
                    self.send_header("Content-Type", kind)
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                def do_POST(self):
                    if self.path != "/v1/audio/speech" or self.headers.get("Origin") or not secrets.compare_digest(self.headers.get("Authorization", ""), "Bearer " + token):
                        self.reply(403, b'{"error":"unauthorized"}')
                        return
                    try:
                        size = int(self.headers.get("Content-Length", "0"))
                        if not 0 < size <= 16384:
                            raise ValueError("请求大小无效")
                        self.connection.settimeout(15)
                        body = self.rfile.read(size)
                        if len(body) != size:
                            raise ValueError("请求不完整")
                        audio = synthesize(*validate_request(body, model_id))
                        self.reply(200, audio, "audio/wav")
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                    except Exception as error:
                        self.reply(400, json.dumps({"error":str(error)[:200]}, ensure_ascii=False).encode("utf-8"))
            server = http.server.HTTPServer(("127.0.0.1", 18767), Handler)
            server.timeout = 0.5
            support.atomic_json(state, dict(state="running", model=model_id, message="离线发音已就绪", pid=os.getpid()))
            try:
                while not stop.exists():
                    server.handle_request()
            finally:
                server.server_close()
            support.atomic_json(state, dict(state="stopped", model=model_id, message="已停止"))
        except Exception as error:
            support.atomic_json(state, dict(state="error", model=model_id, message=str(error)[:300]))
        finally:
            (root / "service-token.txt").unlink(missing_ok=True)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tts', action='store_true')
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--action', choices=['download','serve'], required=True)
    parser.add_argument('--id', required=True)
    args = parser.parse_args()
    model = entry(args.id)
    if model['engine'] == 'external':
        raise ValueError("此模型需要按官方指南外部部署")
    args.root.mkdir(parents=True, exist_ok=True)
    support.CATALOG = CATALOG
    if args.action == 'download':
        support.download(args.root, args.id)
    elif model['engine'] == 'qwen':
        support.run_qwen(args.root, args.id, worker_name='tts_qwen_worker.py')
    else:
        serve(args.root, args.id)

if __name__ == '__main__':
    main()
