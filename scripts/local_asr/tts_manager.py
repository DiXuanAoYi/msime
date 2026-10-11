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
import select
import socket
import sys
import time
import wave
import manager as support
from tts_stream import StreamingSynthesizer, CONTRACT
from tts_options import validate_options

CATALOG = json.loads((support.BASE / "tts_catalog.json").read_text(encoding="utf-8"))

def entry(model_id):
    return next(x for x in CATALOG if x["id"] == model_id)

def nvidia_available():
    if sys.platform != 'win32':
        return False
    try:
        import ctypes
        driver = ctypes.WinDLL('nvcuda.dll')
        count = ctypes.c_int()
        return driver.cuInit(0) == 0 and driver.cuDeviceGetCount(ctypes.byref(count)) == 0 and count.value > 0
    except (OSError, AttributeError):
        return False

def validate_request(body, model_id):
    request = json.loads(body)
    text = request.get("input")
    if not isinstance(text, str) or not text.strip() or len(text.encode("utf-8")) > 2000:
        raise ValueError("朗读文字必须为 1–2000 UTF-8 字节")
    if request.get("model") != model_id or request.get("response_format", "wav") != "wav":
        raise ValueError("请选择正在运行的模型和 WAV 输出")
    if isinstance(request.get('speed'), bool):
        raise ValueError('语速必须是数值')
    speed = float(request.get("speed", 1))
    if not math.isfinite(speed) or not 0.5 <= speed <= 2:
        raise ValueError("语速必须在 0.5–2 之间")
    voice, language = request.get('voice', ''), request.get('language', 'zh')
    if not isinstance(voice, str) or len(voice) > 300 or not isinstance(language, str) or len(language) > 16:
        raise ValueError('音色或语言格式无效')
    model = entry(model_id)
    if model.get('languages') and language not in model['languages']:
        raise ValueError('此模型不支持所选语言：' + language)
    args = text, voice, language, speed
    if 'options' in request:
        return (*args, validate_options(request['options'], entry(model_id)))
    return args

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

def load_kokoro(root, model_id, num_threads=2, provider='cpu'):
    import sherpa_onnx
    directory = root / model_id / "active"
    config = sherpa_onnx.OfflineTtsConfig(
        model=sherpa_onnx.OfflineTtsModelConfig(
            kokoro=sherpa_onnx.OfflineTtsKokoroModelConfig(
                model=str(directory / "model.int8.onnx"), voices=str(directory / "voices.bin"),
                tokens=str(directory / "tokens.txt"), data_dir=str(directory / "espeak-ng-data"),
                dict_dir=str(directory / "dict"),
                lexicon=','.join(str(directory / n) for n in ['lexicon-zh.txt','lexicon-us-en.txt'])),
            num_threads=min(num_threads, os.cpu_count() or 1), provider=provider),
        rule_fsts=','.join(str(directory / n) for n in ['phone-zh.fst','date-zh.fst','number-zh.fst']),
        max_num_sentences=1)
    if not config.validate():
        raise ValueError("Kokoro 运行包不完整")
    model = sherpa_onnx.OfflineTts(config)
    # v1.1's first 53 IDs retain v1.0's named voices.
    voices = {"af_heart":3, "zf_xiaobei":45, "zf_xiaoni":46, "zm_yunjian":49}
    def synthesize(text, voice, language, speed, options, emit, cancelled):
        if language not in ('zh', 'en'):
            raise ValueError("此 Kokoro 运行包支持中文和英文")
        sid = int(voice) if voice.isdecimal() else voices.get(voice, -1)
        if sid < 0 or sid >= model.num_speakers:
            raise ValueError("音色无效，请使用 af_heart / zf_xiaobei 或有效数字音色 ID")
        def callback(samples, progress):
            if cancelled():
                return 0
            emit(samples, model.sample_rate)
            return 1
        model.generate(text, sid=sid, speed=speed, callback=callback)
    return StreamingSynthesizer(synthesize, 'Kokoro '+provider.upper()+' · 逐句播放', accepts_options=True)

def serve(root, model_id, loader=load_kokoro, externally_locked=False, guard=lambda: True):
    state = root / "service.json"
    stop = root / "stop"
    with contextlib.nullcontext() if externally_locked else support.job_lock(root / "service.lock"):
        if not guard():
            return
        if not externally_locked:
            stop.unlink(missing_ok=True)
        token = secrets.token_hex(32)
        (root / "service-token.txt").write_text(token, encoding="ascii")
        support.atomic_json(state, dict(state="loading", model=model_id, message="正在加载发音模型", pid=os.getpid()))
        try:
            loaded = loader(root, model_id)
            if os.environ.get('MSIME_GPU_FALLBACK') and isinstance(loaded, StreamingSynthesizer):
                loaded.device += ' · GPU 不兼容后回退 CPU'
            synthesize = loaded if isinstance(loaded, StreamingSynthesizer) else cache_synthesis(loaded)
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
                    streaming_started = False
                    try:
                        size = int(self.headers.get("Content-Length", "0"))
                        if not 0 < size <= 16384:
                            raise ValueError("请求大小无效")
                        self.connection.settimeout(15)
                        body = self.rfile.read(size)
                        if len(body) != size:
                            raise ValueError("请求不完整")
                        args = validate_request(body, model_id)
                        if json.loads(body).get('stream') is True:
                            if not isinstance(synthesize, StreamingSynthesizer):
                                raise ValueError('此适配器尚不支持流式生成')
                            self.send_response(200)
                            self.send_header('Content-Type', CONTRACT['content_type'])
                            self.send_header('Connection', 'close')
                            self.end_headers()
                            streaming_started = True
                            self.close_connection = True
                            def write(data):
                                self.wfile.write(data)
                                self.wfile.flush()
                            def cancelled():
                                if stop.exists():
                                    return True
                                readable, _, _ = select.select([self.connection], [], [], 0)
                                return bool(readable) and not self.connection.recv(1, socket.MSG_PEEK)
                            metrics = synthesize.stream(args, write, cancelled)
                            support.atomic_json(root / 'last-latency.json', dict(model=model_id, device=synthesize.device, **metrics))
                            support.atomic_json(state,dict(state='running',model=model_id,operation=os.environ.get('MSIME_TTS_OPERATION',''),message='发音已就绪 · '+synthesize.device,pid=os.getpid()))
                        else:
                            audio = synthesize(*args)
                            self.reply(200, audio, "audio/wav")
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                    except Exception as error:
                        if not streaming_started:
                            self.reply(400, json.dumps({"error":str(error)[:200]}, ensure_ascii=False).encode("utf-8"))
                        # After headers, EOF without a terminal frame means failure.
                        self.close_connection = True
            port=int(os.environ.get('MSIME_TTS_PORT','18767'))
            if not 1024<=port<=65535:raise ValueError('本地服务端口无效')
            server = http.server.HTTPServer(("127.0.0.1", port), Handler)
            server.timeout = 0.5
            support.atomic_json(state, dict(state="running", model=model_id, operation=os.environ.get('MSIME_TTS_OPERATION', ''), message="发音已就绪 · " + getattr(synthesize, 'device', 'CPU'), pid=os.getpid()))
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

def switch_service(root, model_id, operation, run):
    """Latest user intent wins, including during dependency setup or model load."""
    def current():
        try:
            desired = json.loads((root / 'desired.json').read_text(encoding='utf-8'))
            return desired.get('operation') == operation and desired.get('model') == model_id
        except (OSError, ValueError):
            return False
    deadline = time.monotonic() + 120
    while current():
        try:
            with support.job_lock(root / 'switch.lock'):
                if not current():
                    return
                (root / 'stop').write_text('switch', encoding='ascii')
                while current():
                    try:
                        with support.job_lock(root / 'service.lock'):
                            break
                    except OSError:
                        if time.monotonic() > deadline:
                            raise TimeoutError('旧模型未能及时停止，请稍后重试')
                        time.sleep(0.1)
            if current():
                os.environ['MSIME_TTS_OPERATION'] = operation
                run(current)
            return
        except PermissionError:
            if time.monotonic() > deadline:
                raise TimeoutError('模型切换等待超时')
            time.sleep(0.1)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tts', action='store_true')
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--action', choices=['download','serve','switch'], required=True)
    parser.add_argument('--id', required=True)
    parser.add_argument('--operation', default='')
    args = parser.parse_args()
    model = entry(args.id)
    if model['engine'] == 'external':
        raise ValueError("此模型需要按官方指南外部部署")
    args.root.mkdir(parents=True, exist_ok=True)
    support.CATALOG = CATALOG
    if args.action == 'download':
        support.download(args.root, args.id)
    else:
        def run(guard=lambda: True):
            if model['engine']=='kokoro' and nvidia_available():
                support.run_qwen(args.root,args.id,worker_name='tts_kokoro_cuda_worker.py',guard=guard)
                return
            if model['engine'] in ('qwen', 'moss', 'edge', 'voxtral', 'mms'):
                worker = {'moss': 'tts_moss_worker.py', 'qwen': 'tts_qwen_worker.py', 'edge': 'tts_edge_worker.py', 'voxtral': 'tts_voxtral_worker.py', 'mms':'tts_mms_worker.py'}[model['engine']]
                if model['engine'] == 'qwen' and nvidia_available():
                    worker = 'tts_qwen_cuda_worker.py'
                if model['engine'] == 'moss' and nvidia_available():
                    worker = 'tts_moss_cuda_worker.py'
                if model['engine'] == 'mms' and nvidia_available():
                    worker = 'tts_mms_cuda_worker.py'
                support.run_qwen(args.root, args.id, worker_name=worker, guard=guard)
            else:
                serve(args.root, args.id, guard=guard)
        if args.action == 'switch':
            try:
                switch_service(args.root, args.id, args.operation, run)
            except Exception as error:
                support.atomic_json(args.root / 'switch-error.json', dict(operation=args.operation, message=str(error)[:240]))
        else:
            run()

if __name__ == '__main__':
    main()
