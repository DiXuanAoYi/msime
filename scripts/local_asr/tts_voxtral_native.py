"""Pinned audio.cpp C ABI bridge. Streaming events contain NEW float PCM only."""
import ctypes as C
from pathlib import Path
import numpy as np


class Backend(C.Structure):
    _fields_ = [('backend', C.c_char_p), ('device', C.c_int), ('threads', C.c_int)]


class ModelConfig(C.Structure):
    _fields_ = [(name, C.c_char_p) for name in ('family_hint', 'config_id', 'weight_id', 'model_spec_override')]


class VoxtralNative:
    def __init__(self, dll, model_path, threads=4):
        self.lib = C.CDLL(str(dll))
        self.registry = C.c_void_p()
        self.model = C.c_void_p()
        self.session = C.c_void_p()
        p, pp = C.c_void_p, C.POINTER(C.c_void_p)
        specs = {
            'abi_version': (C.c_uint32, []), 'last_error': (C.c_char_p, []),
            'registry_create': (C.c_int, [C.c_char_p, pp]), 'registry_free': (None, [p]),
            'model_load': (C.c_int, [p, C.c_char_p, C.POINTER(ModelConfig), p, pp]), 'model_free': (None, [p]),
            'session_create': (C.c_int, [p, C.c_char_p, C.c_char_p, C.POINTER(Backend), p, pp]),
            'session_free': (None, [p]), 'request_create': (p, []), 'request_free': (None, [p]),
            'request_set_text': (C.c_int, [p, C.c_char_p, C.c_char_p]),
            'request_set_voice_id': (C.c_int, [p, C.c_char_p]),
            'request_set_option': (C.c_int, [p, C.c_char_p, C.c_char_p]),
            'stream_start': (C.c_int, [p, p]), 'stream_next_event': (C.c_int, [p, pp]),
            'stream_reset': (C.c_int, [p]), 'stream_finish': (C.c_int, [p, pp]),
            'event_is_final': (C.c_int, [p]), 'event_as_result': (p, [p]), 'event_free': (None, [p]),
            'result_free': (None, [p]),
            'result_audio': (C.c_int, [p, C.POINTER(C.POINTER(C.c_float)), C.POINTER(C.c_size_t), C.POINTER(C.c_int), C.POINTER(C.c_int)]),
        }
        for name, (returns, args) in specs.items():
            fn = getattr(self.lib, 'audiocpp_' + name)
            fn.restype, fn.argtypes = returns, args
            setattr(self, name, fn)
        self.callback_type = C.CFUNCTYPE(C.c_int, C.POINTER(C.c_float), C.c_size_t, C.c_int, C.c_int, C.c_void_p)
        self.lib.msime_voxtral_stream.restype = C.c_int
        self.lib.msime_voxtral_stream.argtypes = [p, p, self.callback_type, p]
        if self.abi_version() >> 16 != 0 or self.abi_version() < 0x200:
            raise RuntimeError('Voxtral 运行组件 ABI 不匹配，请更新安装包')
        try:
            self.check(self.registry_create(None, C.byref(self.registry)))
            config = ModelConfig(b'voxtral_tts', None, None, None)
            self.check(self.model_load(self.registry, str(model_path).encode('utf-8'), C.byref(config), None, C.byref(self.model)))
            backend = Backend(b'cpu', 0, threads)
            self.check(self.session_create(self.model, b'tts', b'streaming', C.byref(backend), None, C.byref(self.session)))
        except Exception:
            self.close()
            raise

    def check(self, code):
        if code:
            raise RuntimeError(self.last_error().decode('utf-8', 'replace')[:400])

    def close(self):
        for name in ('session', 'model', 'registry'):
            handle = getattr(self, name, None)
            if handle:
                getattr(self, name + '_free')(handle)
                setattr(self, name, C.c_void_p())

    def generate(self, text, voice, language, speed, options, emit, cancelled):
        request = self.request_create()
        if not request:
            raise MemoryError('无法分配 Voxtral 请求')
        started = False
        try:
            self.check(self.request_set_text(request, text.encode('utf-8'), None))
            self.check(self.request_set_voice_id(request, voice.encode('utf-8')))
            for key, value in {'max_tokens': 1125, **options}.items():
                if key == 'seed' and value == -1:
                    continue
                self.check(self.request_set_option(request, key.encode('ascii'), str(value).encode('ascii')))
            if cancelled():
                raise InterruptedError('已停止朗读')
            errors = []
            @self.callback_type
            def callback(samples, frames, rate, channels, opaque):
                try:
                    if cancelled():
                        raise InterruptedError('已停止朗读')
                    if not 1 <= channels <= 2 or not 0 < frames <= 48000 * 90:
                        raise ValueError('Voxtral 音频格式或长度无效')
                    audio = np.ctypeslib.as_array(samples, shape=(frames * channels,)).reshape(-1, channels).copy()
                    if speed != 1:
                        audio = np.stack([np.interp(np.arange(0, len(audio), speed), np.arange(len(audio)), c) for c in audio.T], axis=1)
                    emit(audio, rate)
                    return 1
                except Exception as error:
                    errors.append(error)
                    return 0
            started = True
            code = self.lib.msime_voxtral_stream(self.session, request, callback, None)
            if errors:
                raise errors[0]
            self.check(code)
        finally:
            if started:
                self.stream_reset(self.session)
            self.request_free(request)
