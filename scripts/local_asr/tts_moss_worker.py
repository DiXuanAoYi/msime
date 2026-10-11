# /// script
# requires-python = ">=3.12,<3.13"
# dependencies = ["numpy==2.3.5", "onnxruntime==1.23.2", "sentencepiece==0.2.1"]
# ///
"""MOSS ONNX built-in voices, genuinely incremental codec decoding on CPU."""
from pathlib import Path
import sys
import numpy as np
import sentencepiece as spm
sys.path.insert(0, str(Path(__file__).parent / 'vendor/moss'))
from ort_cpu_runtime import OrtCpuRuntime
from tts_stream import StreamingSynthesizer
from tts_manager import serve
from tts_options import moss_generation_options

class Runtime(OrtCpuRuntime):
    def _create_sessions(self):
        files = self.tts_meta['files']
        paths = {'prefill': files['prefill'], 'decode': files['decode_step'], 'local_decoder': files['local_decoder']}
        for name in ('local_fixed_sampled_frame', 'local_cached_step', 'local_greedy_frame'):
            if files.get(name):
                paths[name] = files[name]
        sessions = {k: self._session(self.tts_meta_path.parent / v) for k, v in paths.items()}
        sessions['codec_decode_step'] = self._session(self.codec_meta_path.parent / self.codec_meta['files']['decode_step'])
        return sessions

def load(root, model_id, prefer_gpu=False):
    device = 'ONNX CPU · 流式'
    if prefer_gpu:
        try:
            runtime = Runtime(root / model_id / 'original', thread_count=4, max_new_frames=1125, execution_provider='cuda')
            device = 'ONNX CUDA 优先 · 不支持的算子回退 CPU · 流式'
        except Exception as error:
            print('MOSS GPU unavailable; CPU fallback:', str(error)[:300], file=sys.stderr, flush=True)
            runtime = Runtime(root / model_id / 'original', thread_count=4, max_new_frames=1125)
            device = 'ONNX CPU · GPU 初始化失败后回退 · 流式'
    else:
        runtime = Runtime(root / model_id / 'original', thread_count=4, max_new_frames=1125)
    tokenizer = spm.SentencePieceProcessor(model_file=str(runtime.resolve_manifest_relative_path(runtime.manifest['model_files']['tokenizer_model'])))
    voices = {v['voice']: v for v in runtime.list_builtin_voices()}
    defaults = dict(runtime.manifest['generation_defaults'])
    def generate(text, voice, language, speed, options, emit, cancelled):
        # The optimized fixed-sampling ONNX graph bakes in its sampling values.
        # Use the cached-step path when controls differ, rather than silently
        # accepting UI parameters that the fixed graph would ignore.
        runtime.manifest['generation_defaults'] = moss_generation_options(defaults, options)
        selected = voice
        if selected in ('', 'default'):
            selected = 'Xiaoyu' if language == 'zh' else 'Bella'
        if selected not in voices:
            raise ValueError('音色无效，可选：' + ', '.join(voices))
        # Short candidate words need an utterance boundary, otherwise this model
        # sometimes continues the reference speaker's sentence or generates silence.
        spoken = text.strip()
        if spoken[-1] not in '.!?。！？;；':
            spoken += '。' if language == 'zh' else '.'
        rows = runtime.build_voice_clone_request_rows(voices[selected]['prompt_audio_codes'], tokenizer.encode(spoken, out_type=int))
        pending = []
        rate = int(runtime.codec_meta['codec_config']['sample_rate'])
        def flush():
            if not pending:
                return
            decoded = runtime.codec_streaming_session.run_frames(pending)
            pending.clear()
            if decoded is not None:
                audio, length = decoded
                if length > 0:
                    samples = audio[0, :, :length].T
                    if speed != 1:
                        samples = np.stack([np.interp(np.arange(0, length, speed), np.arange(length), ch) for ch in samples.T], axis=1)
                    emit(samples, rate)
        def on_frame(frames, step, frame):
            if cancelled():
                raise InterruptedError('已停止朗读')
            pending.append(frame)
            if len(pending) >= options.get('chunk_frames', 2):
                flush()
        runtime.codec_streaming_session.reset()
        try:
            runtime.generate_audio_frames(rows, on_frame=on_frame)
            flush()
        finally:
            runtime.codec_streaming_session.reset()
    if prefer_gpu and runtime.execution_provider == 'cuda':
        # Prime kernels without playing or retaining a spoken-text cache. This also
        # resolves ONNX's per-session CPU fallback before reporting readiness.
        generate('你好。', 'Xiaoyu', 'zh', 1, {'max_new_frames':12}, lambda *args:None, lambda:False)
        gpu = [name for name, session in runtime.sessions.items() if 'CUDAExecutionProvider' in session.get_providers()]
        cpu = [name for name, session in runtime.sessions.items() if 'CUDAExecutionProvider' not in session.get_providers()]
        device = 'ONNX CUDA · 流式' if gpu and not cpu else ('ONNX CUDA + CPU 回退 · 流式' if gpu else 'ONNX CPU · GPU 算子不可用后回退 · 流式')
    return StreamingSynthesizer(generate, device, accepts_options=True)

if __name__ == '__main__':
    serve(Path(sys.argv[1]), sys.argv[2], load, externally_locked=True)
