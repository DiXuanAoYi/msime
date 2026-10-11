"""Local ASR worker. Frozen with its CPU runtime; no chat or microphone access."""
from __future__ import annotations
import argparse
import array
import contextlib
import email.parser
import email.policy
import hashlib
import http.server
import io
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import tarfile
import threading
import time
import urllib.request
import wave

BASE = Path(getattr(sys, "_MEIPASS", Path(__file__).parent))
CATALOG = json.loads((BASE / "catalog.json").read_text(encoding="utf-8"))
MAX_AUDIO = 20 * 1024 * 1024


def atomic_json(path, value):
    path = Path(path)
    if path.name == 'service.json' and os.environ.get('MSIME_TTS_OPERATION'):
        value = dict(value, operation=os.environ['MSIME_TTS_OPERATION'])
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + f".{os.getpid()}.tmp")
    tmp.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
    # Windows readers/virus scanners can briefly hold a non-delete-sharing handle.
    for attempt in range(30):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if attempt == 29:
                raise
            time.sleep(0.025)


def model_entry(model_id):
    return next(x for x in CATALOG if x["id"] == model_id)


def digest(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def safe_extract(archive, target, limit, cancelled=lambda: False):
    """No links/devices/absolute paths or path traversal, even in a pinned package."""
    target = Path(target).resolve()
    total = 0
    with tarfile.open(archive, "r:bz2") as tar:
        for member in tar:
            if cancelled():
                raise InterruptedError("已暂停；再次下载将继续校验和展开")
            path = (target / member.name).resolve()
            if not path.is_relative_to(target) or "\\" in member.name or ":" in member.name:
                raise ValueError("运行包包含不安全路径")
            if not member.isdir() and not member.isfile():
                raise ValueError("运行包包含非普通文件")
            total += member.size
            if total > limit:
                raise ValueError("运行包展开大小超出限制")
            if member.isdir():
                path.mkdir(parents=True, exist_ok=True)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(member) as source, path.open("wb") as dest:
                    shutil.copyfileobj(source, dest, 1024 * 1024)


@contextlib.contextmanager
def job_lock(path):
    import msvcrt
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as lock:
        lock.seek(0)
        if not lock.read(1):
            lock.write(b"0")
            lock.flush()
        lock.seek(0)
        msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        try:
            yield
        finally:
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)


def download(root, model_id):
    entry = model_entry(model_id)
    dest = root / model_id
    dest.mkdir(parents=True, exist_ok=True)
    state = dest / "state.json"
    cancel = dest / "cancel"
    done = 0
    def report(stage, detail=""):
        atomic_json(state, dict(state=stage, downloaded=done, total=entry["downloadBytes"],
                                message=detail, updated=time.time(), pid=os.getpid()))
    try:
        with job_lock(root / "download.lock"):
            cancel.unlink(missing_ok=True)
            if shutil.disk_usage(root).free < entry["diskBytes"]:
                raise ValueError("剩余磁盘空间不足，请至少预留下载量的 2–3 倍空间")
            report("downloading")
            for file in entry["files"]:
                if cancel.exists():
                    raise InterruptedError("已暂停；再次下载将续传")
                target = dest / ("original" if entry["engine"] in ('qwen', 'moss') else "archives") / file["name"]
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.exists() and target.stat().st_size == file["bytes"] and digest(target) == file["sha256"]:
                    done += file["bytes"]
                    continue
                part = target.with_suffix(target.suffix + ".part")
                offset = part.stat().st_size if part.exists() else 0
                if offset > file["bytes"]:
                    part.unlink()
                    offset = 0
                headers = {"User-Agent": "MetasequoiaIME-LocalASR/1"}
                if offset:
                    headers["Range"] = f"bytes={offset}-"
                if offset < file["bytes"]:
                    with urllib.request.urlopen(urllib.request.Request(file["url"], headers=headers), timeout=30) as response:
                        if response.status != 206:
                            offset = 0
                        elif not response.headers.get("Content-Range", "").startswith(f"bytes {offset}-"):
                            raise ValueError("下载服务器返回了错误的续传范围")
                        done += offset
                        last = 0
                        with part.open("ab" if offset else "wb") as output:
                            while chunk := response.read(256 * 1024):
                                if cancel.exists():
                                    report("paused", "已暂停；再次下载将续传")
                                    return
                                if output.tell() + len(chunk) > file["bytes"]:
                                    raise ValueError("模型文件大小与锁定清单不符")
                                output.write(chunk)
                                done += len(chunk)
                                if time.monotonic() - last > 0.4:
                                    report("downloading")
                                    last = time.monotonic()
                else:
                    done += offset
                report("verifying", "正在校验 SHA-256")
                if part.stat().st_size < file["bytes"]:
                    raise InterruptedError("下载连接提前结束；已保留进度，再次下载将续传")
                if part.stat().st_size != file["bytes"] or digest(part) != file["sha256"]:
                    part.unlink(missing_ok=True)
                    raise ValueError("模型校验失败，损坏文件已移除，请重新下载")
                os.replace(part, target)
            if entry["engine"] not in ('qwen', 'moss'):
                report("extracting", "校验通过，正在展开运行包")
                staging = dest / "unpacking"
                safe_extract(dest / "archives" / entry["files"][0]["name"], staging, entry["diskBytes"] * 2, cancel.exists)
                source = staging / entry["directory"]
                if not source.is_dir():
                    raise ValueError("运行包目录与清单不一致")
                active = dest / "active"
                if active.exists():
                    shutil.rmtree(active)
                os.replace(source, active)
            atomic_json(dest / "ready.json", {"files": entry["files"], "engine": entry["engine"]})
            report("ready", "模型已下载并校验，可启动本地识别")
    except InterruptedError as error:
        report("paused", str(error))
    except Exception as error:
        report("error", str(error)[:300])


def sherpa_recognizer(root, model_id):
    import sherpa_onnx
    entry = model_entry(model_id)
    directory = root / model_id / "active"
    def find(pattern):
        matches = sorted(directory.glob(pattern))
        if len(matches) != 1:
            raise ValueError("模型文件缺失或重复：" + pattern)
        return str(matches[0])
    args = dict(num_threads=max(1, min(4, os.cpu_count() or 1)), provider="cpu")
    engine = entry["engine"]
    if engine == "sensevoice":
        recognizer = sherpa_onnx.OfflineRecognizer.from_sense_voice(
            model=find("model.int8.onnx"), tokens=find("tokens.txt"), language="auto", use_itn=True, **args)
    elif engine == "parakeet":
        recognizer = sherpa_onnx.OfflineRecognizer.from_transducer(
            encoder=find("encoder*.onnx"), decoder=find("decoder*.onnx"), joiner=find("joiner*.onnx"),
            tokens=find("tokens.txt"), model_type="nemo_transducer", **args)
    elif engine == "firered":
        recognizer = sherpa_onnx.OfflineRecognizer.from_fire_red_asr(
            encoder=find("encoder*.onnx"), decoder=find("decoder*.onnx"), tokens=find("tokens.txt"), **args)
    else:
        recognizer = sherpa_onnx.OfflineRecognizer.from_qwen3_asr(
            conv_frontend=find("conv_frontend*.onnx"), encoder=find("encoder*.onnx"),
            decoder=find("decoder*.onnx"), tokenizer=str(directory / "tokenizer"), **args)
    def transcribe(samples):
        stream = recognizer.create_stream()
        stream.accept_waveform(16000, samples)
        recognizer.decode_stream(stream)
        return stream.result.text
    return transcribe


def decode_upload(body, content_type, expected_model):
    message = email.parser.BytesParser(policy=email.policy.default).parsebytes(
        ("Content-Type: " + content_type + "\r\nMIME-Version: 1.0\r\n\r\n").encode("ascii") + body)
    fields = {}
    if not message.is_multipart():
        raise ValueError("需要 multipart/form-data 音频")
    for part in message.iter_parts():
        name = part.get_param("name", header="content-disposition")
        if name in fields:
            raise ValueError("重复字段")
        fields[name] = part.get_payload(decode=True)
    if fields.get("model", b"").decode("utf-8") != expected_model:
        raise ValueError("选择的模型与运行中的本地模型不同，请先启动所选模型")
    with wave.open(io.BytesIO(fields["file"]), "rb") as wav:
        if wav.getnchannels() != 1 or wav.getframerate() != 16000 or wav.getsampwidth() != 2:
            raise ValueError("仅接受 16 kHz 单声道 PCM16 WAV")
        if wav.getnframes() > 60 * 16000:
            raise ValueError("本地识别每段最多 60 秒")
        pcm = array.array("h", wav.readframes(wav.getnframes()))
        return [x / 32768.0 for x in pcm]


def serve(root, model_id, loader=sherpa_recognizer, externally_locked=False):
    state = root / "service.json"
    stop = root / "stop"
    with contextlib.nullcontext() if externally_locked else job_lock(root / "service.lock"):
        if not externally_locked:
            stop.unlink(missing_ok=True)
        token = secrets.token_hex(32)
        (root / "service-token.txt").write_text(token, encoding="ascii")
        atomic_json(state, dict(state="loading", model=model_id, message="正在加载模型", pid=os.getpid()))
        try:
            transcribe = loader(root, model_id)
            class Handler(http.server.BaseHTTPRequestHandler):
                timeout = 15
                def log_message(self, *args):
                    pass  # Never log audio, tokens or transcripts.
                def reply(self, status, value):
                    data = json.dumps(value, ensure_ascii=False).encode("utf-8")
                    self.send_response(status)
                    self.send_header("Content-Type", "application/json; charset=utf-8")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                def do_POST(self):
                    if self.path != "/v1/audio/transcriptions" or self.headers.get("Origin") or not secrets.compare_digest(self.headers.get("Authorization", ""), "Bearer " + token):
                        self.reply(403, {"error": "本机识别凭据无效"})
                        return
                    try:
                        size = int(self.headers.get("Content-Length", "0"))
                        if size <= 0 or size > MAX_AUDIO:
                            raise ValueError("音频大小无效")
                        self.connection.settimeout(15)
                        body = self.rfile.read(size)
                        if len(body) != size:
                            raise ValueError("音频数据不完整")
                        samples = decode_upload(body, self.headers.get("Content-Type", ""), model_id)
                        self.reply(200, {"text": transcribe(samples)})
                    except Exception as error:
                        self.reply(400, {"error": str(error)[:240]})
            server = http.server.HTTPServer(("127.0.0.1", 18766), Handler)
            server.timeout = 0.5
            atomic_json(state, dict(state="running", model=model_id, message="本地识别已就绪", pid=os.getpid()))
            try:
                while not stop.exists():
                    server.handle_request()
            finally:
                server.server_close()
            atomic_json(state, dict(state="stopped", model=model_id, message="已停止"))
        except Exception as error:
            atomic_json(state, dict(state="error", model=model_id, message=str(error)[:300]))
        finally:
            (root / "service-token.txt").unlink(missing_ok=True)


def run_qwen(root, model_id, worker_name="qwen_worker.py", guard=lambda: True):
    """Keep a single service slot through dependency preparation and model loading."""
    with job_lock(root / "service.lock"):
        if not guard():
            return
        stop = root / "stop"
        stop.unlink(missing_ok=True)
        atomic_json(root / "service.json", dict(state="loading", model=model_id,
            message="首次启动正在准备模型运行依赖（额外下载，不计入模型大小）", pid=os.getpid()))
        env = dict(os.environ, UV_CACHE_DIR=str(root / "runtime-cache"),
                   HF_HUB_OFFLINE="1", TRANSFORMERS_OFFLINE="1")
        worker = BASE / "worker" if getattr(sys, "frozen", False) else BASE
        command = [str(BASE / "uv.exe"), "run", "--python", "3.12", "--no-project", "--script",
                   str(worker / worker_name), str(root), model_id]
        try:
            if getattr(sys, "frozen", False) and sys.platform == "win32":
                # PyInstaller's DLL search directory must not leak into another Python ABI.
                import ctypes
                ctypes.windll.kernel32.SetDllDirectoryW(None)
            with (root / "runtime-install.log").open("w", encoding="utf-8") as output:
                child = subprocess.Popen(command, env=env, stdout=output, stderr=output,
                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                while child.poll() is None:
                    if stop.exists():
                        # taskkill is limited to this owned worker tree; never terminate other Python processes.
                        subprocess.run([str(Path(os.environ["SystemRoot"]) / "System32" / "taskkill.exe"),
                                        "/PID", str(child.pid), "/T", "/F"],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                        child.wait(timeout=15)
                        atomic_json(root / "service.json", dict(state="stopped", model=model_id, message="已停止"))
                        return
                    time.sleep(0.5)
                if child.returncode:
                    raise RuntimeError("模型运行依赖安装或启动失败，详情见 runtime-install.log")
        except Exception as error:
            atomic_json(root / "service.json", dict(state="error", model=model_id, message=str(error)[:300]))
        finally:
            (root / "service-token.txt").unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--action", choices=["download", "serve"], required=True)
    parser.add_argument("--id", required=True)
    args = parser.parse_args()
    model_entry(args.id)
    args.root.mkdir(parents=True, exist_ok=True)
    if args.action == "download":
        download(args.root, args.id)
    elif model_entry(args.id)["engine"] == "qwen":
        run_qwen(args.root, args.id)
    else:
        serve(args.root, args.id)


if __name__ == "__main__":
    if '--tts' in sys.argv:
        import tts_manager
        tts_manager.main()
    else:
        main()
