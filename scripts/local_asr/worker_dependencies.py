"""Check native torch ABI compatibility before loading extension DLLs."""
import importlib.metadata
import sys

def validate_torch_audio(versions=None):
    if sys.platform == 'win32':
        import ctypes
        # Report loader failures in the service log instead of a modal Python error.
        ctypes.windll.kernel32.SetErrorMode(0x8003)
    versions = versions or {name: importlib.metadata.version(name) for name in ('torch', 'torchaudio')}
    releases = [versions[name].split('+', 1)[0] for name in ('torch', 'torchaudio')]
    if releases != ['2.9.1', '2.9.1']:
        raise RuntimeError('Qwen 需要配套的 torch 2.9.1 / torchaudio 2.9.1，请更新运行组件后重新启动模型。')
