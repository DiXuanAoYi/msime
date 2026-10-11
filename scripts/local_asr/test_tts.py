import io
import json
import unittest
import wave
from pathlib import Path
import tomllib
import tts_manager as tts
from worker_dependencies import validate_torch_audio

class LocalTtsTests(unittest.TestCase):
    def test_catalog_has_sizes_and_only_three_managed_downloads(self):
        self.assertEqual(len(tts.CATALOG), 9)
        managed=[m for m in tts.CATALOG if m['engine']!='external']
        self.assertEqual(len(managed), 3)
        for m in tts.CATALOG:
            self.assertGreater(m['downloadBytes'], 0)
            self.assertTrue(m['official'].startswith('https://'))
        for m in managed:
            self.assertEqual(sum(f['bytes'] for f in m['files']), m['downloadBytes'])
            for f in m['files']:
                self.assertRegex(f['sha256'], r'^[0-9a-f]{64}$')
                self.assertNotIn('/resolve/main/', f['url'])

    def test_request_rejects_wrong_model_long_text_and_invalid_speed(self):
        request={'input':'你好','model':'kokoro','voice':'45','language':'zh','speed':1,'response_format':'wav'}
        self.assertEqual(tts.validate_request(json.dumps(request), 'kokoro'),('你好','45','zh',1))
        for patch in [{'input':''},{'input':'a'*2001},{'speed':float('nan')},{'speed':3},{'model':'other'},{'response_format':'mp3'}]:
            with self.assertRaises(ValueError):
                tts.validate_request(json.dumps({**request,**patch}), 'kokoro')

    def test_audio_is_clipped_pcm16_wav(self):
        data=tts.pcm_wav([-2,0,2],24000)
        with wave.open(io.BytesIO(data),'rb') as wav:
            self.assertEqual(wav.getparams()[:4],(1,2,24000,3))
            self.assertEqual(wav.readframes(3),b'\x01\x80\0\0\xff\x7f')

    def test_qwen_rejects_native_abi_mismatch_before_import(self):
        validate_torch_audio({'torch':'2.9.1+cpu', 'torchaudio':'2.9.1+cpu'})
        for audio in ['2.11.0', '2.9.0']:
            with self.assertRaisesRegex(RuntimeError, '2.9.1'):
                validate_torch_audio({'torch':'2.9.1', 'torchaudio':audio})
        for worker in ['qwen_worker.py', 'tts_qwen_worker.py']:
            source=Path(__file__).with_name(worker).read_text(encoding='utf-8')
            metadata=source.split('# /// script\n',1)[1].split('# ///',1)[0]
            requirements=tomllib.loads('\n'.join(line.removeprefix('# ') for line in metadata.splitlines()))['dependencies']
            self.assertIn('torch==2.9.1', requirements)
            self.assertIn('torchaudio==2.9.1', requirements)
            self.assertLess(source.index('validate_torch_audio()'), source.index('import torch'))

    def test_cache_reuses_audio_but_separates_voice_language_and_speed(self):
        calls=[]
        def synth(*args):
            calls.append(args)
            return str(args).encode()
        cached=tts.cache_synthesis(synth)
        args=('你好','45','zh',1)
        expected=cached(*args)
        self.assertIs(cached(*args), expected)
        for changed in [('再见','45','zh',1),('你好','46','zh',1),('你好','45','en',1),('你好','45','zh',1.2)]:
            cached(*changed)
        self.assertEqual(len(calls),5)
        # A new model/service gets an empty cache.
        tts.cache_synthesis(synth)(*args)
        self.assertEqual(len(calls),6)

    def test_cache_enforces_byte_and_entry_budgets_and_lru_order(self):
        for budget, entries in [(4,100),(100,2)]:
            calls=[]
            def synth(text,*args):
                calls.append(text)
                return text.encode()*2
            cached=tts.cache_synthesis(synth,max_bytes=budget,max_entries=entries)
            for text in ['a','b','a','c','a','b']:
                cached(text,'v','zh',1)
            self.assertEqual(calls,['a','b','c','b'])
        calls=[]
        def synth(text,*args):
            calls.append(text)
            if text=='error':
                raise ValueError('failed')
            return b'12345'
        cached=tts.cache_synthesis(synth,max_bytes=4)
        for _ in range(2):
            cached('large','v','zh',1)
            with self.assertRaises(ValueError):
                cached('error','v','zh',1)
        self.assertEqual(calls,['large','error','large','error'])

if __name__=='__main__':
    unittest.main()
