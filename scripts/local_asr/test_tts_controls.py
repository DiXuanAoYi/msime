import ctypes
import hashlib
import http.server
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch
import manager
from tts_options import moss_generation_options
from tts_voxtral_convert import prepare_model


class TtsControlsTests(unittest.TestCase):
    def test_moss_controls_select_a_path_that_actually_applies_them(self):
        default={'do_sample':True,'sample_mode':'fixed','audio_temperature':0.8,'audio_top_k':25}
        self.assertEqual(moss_generation_options(default,{'chunk_frames':3})['sample_mode'],'fixed')
        self.assertNotIn('chunk_frames',moss_generation_options(default,{'chunk_frames':3}))
        self.assertEqual(moss_generation_options(default,{'audio_temperature':0.8})['sample_mode'],'fixed')
        self.assertEqual(moss_generation_options(default,{'audio_temperature':0.6})['sample_mode'],'full')
        self.assertEqual(moss_generation_options(default,{'do_sample':False})['sample_mode'],'greedy')
        self.assertEqual(default['sample_mode'],'fixed')

    def test_voxtral_downloads_only_on_explicit_action_and_uses_raw_layout(self):
        payload=b'synthetic fixture, not a model'
        requests=[]
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_GET(self):
                requests.append(self.path);self.send_response(200);self.send_header('Content-Length',str(len(payload)));self.end_headers();self.wfile.write(payload)
        server=http.server.HTTPServer(('127.0.0.1',0),Handler)
        thread=threading.Thread(target=server.serve_forever);thread.start()
        try:
            row=dict(id='voxtral-test',engine='voxtral',diskBytes=10000,downloadBytes=len(payload),files=[dict(name='voice_embedding/test.pt',bytes=len(payload),sha256=hashlib.sha256(payload).hexdigest(),url=f'http://127.0.0.1:{server.server_port}/fixture')])
            with tempfile.TemporaryDirectory() as temporary,patch.object(manager,'CATALOG',[row]):
                root=Path(temporary);self.assertEqual(requests,[])
                manager.download(root,'voxtral-test')
                self.assertEqual((root/'voxtral-test/original/voice_embedding/test.pt').read_bytes(),payload)
                self.assertEqual(json.loads((root/'voxtral-test/state.json').read_text())['state'],'ready')
                self.assertFalse((root/'voxtral-test/archives').exists())
                manager.download(root,'voxtral-test')
                self.assertEqual(requests,['/fixture'])
        finally:
            server.shutdown();thread.join();server.server_close()

    def test_missing_voxtral_weights_never_trigger_implicit_model_download(self):
        with tempfile.TemporaryDirectory() as directory, patch('urllib.request.urlopen',side_effect=AssertionError('unexpected network')):
            with self.assertRaisesRegex(ValueError,'先下载'):
                prepare_model(Path(directory),Path(directory)/'native')

    def test_built_native_runtime_abi_and_missing_model_cleanup(self):
        dll=Path(__file__).parent/'native-voxtral/audiocpp.dll'
        if not dll.is_file():self.skipTest('Run Build-Voxtral.ps1 for the native ABI smoke test')
        from tts_voxtral_native import VoxtralNative
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(RuntimeError):VoxtralNative(dll,Path(directory)/'absent.gguf')
        lib=ctypes.CDLL(str(dll))
        lib.audiocpp_abi_version.restype=ctypes.c_uint32
        self.assertGreaterEqual(lib.audiocpp_abi_version(),0x200)
        self.assertTrue(hasattr(lib,'msime_voxtral_stream'))


if __name__=='__main__':unittest.main()
