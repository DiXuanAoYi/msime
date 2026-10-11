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

def load(root, model_id):
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
    return StreamingSynthesizer(generate, 'ONNX CPU · 流式', accepts_options=True)

if __name__ == '__main__':
    serve(Path(sys.argv[1]), sys.argv[2], load, externally_locked=True)
