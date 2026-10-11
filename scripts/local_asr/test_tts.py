import io
import json
import unittest
import wave
from pathlib import Path
import tomllib
import struct
from tts_options import validate_options
from tts_stream import StreamingSynthesizer, CONTRACT
import tts_manager as tts
from worker_dependencies import validate_torch_audio

class LocalTtsTests(unittest.TestCase):
    def test_controls_reject_unsupported_values_and_only_qwen_17_accepts_instructions(self):
        qwen = tts.entry('qwen-0.6b')
        self.assertEqual(json.loads(validate_options({'temperature':0.7,'do_sample':False},qwen)), {'temperature':0.7,'do_sample':False})
        for value in [{'temperature':True},{'temperature':float('nan')},{'temperature':99},{'chunk_size':1.5},{'instruct':'unused'},{'endpoint':'https://example.com'}]:
            with self.assertRaises(ValueError):validate_options(value,qwen)
        self.assertEqual(json.loads(validate_options({'instruct':'温柔地说'},tts.entry('qwen-1.7b')))['instruct'],'温柔地说')
        with self.assertRaises(ValueError):
            tts.validate_request(json.dumps({'input':'text','model':'voxtral-tts','language':'zh'}),'voxtral-tts')

    def test_model_options_reach_generation_and_separate_cache_keys(self):
        received=[]
        def generate(text,voice,language,speed,options,emit,cancelled):
            received.append(options);emit([options['temperature']/10],24000)
        synth=StreamingSynthesizer(generate,accepts_options=True)
        args=('hello','v','en',1)
        first=synth(*args,'{"temperature":0.5}')
        self.assertEqual(synth(*args,'{"temperature":0.5}'),first)
        self.assertNotEqual(synth(*args,'{"temperature":0.8}'),first)
        self.assertEqual(received,[{'temperature':0.5},{'temperature':0.8}])

    def test_explicit_random_seed_does_not_reuse_cached_audio(self):
        calls=[]
        def generate(text,voice,language,speed,options,emit,cancelled):
            calls.append(1);emit([len(calls)/10],24000)
        synth=StreamingSynthesizer(generate,accepts_options=True)
        a=synth('a','v','en',1,'{"seed":-1}')
        self.assertNotEqual(synth('a','v','en',1,'{"seed":-1}'),a)
        self.assertEqual(len(calls),2)

    def test_catalog_has_sizes_and_pinned_managed_downloads(self):
        managed=[m for m in tts.CATALOG if m['engine'] not in ('external', 'edge')]
        self.assertEqual({m['id'] for m in managed}, {'kokoro','qwen-0.6b','qwen-1.7b','moss-nano','voxtral-tts','mms-bod','mms-kaz'})
        for m in tts.CATALOG:
            if m in managed:
                self.assertGreater(m['downloadBytes'], 0)
            self.assertTrue(m['official'].startswith('https://'))
        for m in managed:
            self.assertEqual(sum(f['bytes'] for f in m['files']), m['downloadBytes'])
            for f in m['files']:
                self.assertRegex(f['sha256'], r'^[0-9a-f]{64}$')
                self.assertNotIn('/resolve/main/', f['url'])

    def test_stream_emits_before_generation_finishes_and_replays_complete_cache(self):
        events, frames = [], []
        def generate(text, voice, language, speed, emit, cancelled):
            emit([0.1, -0.1], 24000)
            self.assertEqual(events, ['write', 'write'])
            events.append('generation-continues')
            emit([0.2, 0.3], 24000)
        model = StreamingSynthesizer(generate)
        def write(frame):
            events.append('write')
            frames.append(frame)
        args = ('test', 'voice', 'zh', 1)
        model.stream(args, write)
        self.assertEqual(frames[0], CONTRACT['magic'].encode() + struct.pack('<II', 24000, 1))
        self.assertEqual(frames[-1], b'\0'*4)
        replay = []
        self.assertTrue(model.stream(args, replay.append)['cached'])
        self.assertEqual(replay, frames)
        with wave.open(io.BytesIO(model(*args))) as wav:
            self.assertEqual(wav.getnframes(), 4)

    def test_stream_failure_and_cancel_never_cache_or_emit_success_marker(self):
        def failing(text, voice, language, speed, emit, cancelled):
            emit([0.1], 24000)
            raise ValueError('model failed mid-generation')
        model = StreamingSynthesizer(failing)
        frames = []
        with self.assertRaises(ValueError):
            model.stream(('test', 'v', 'zh', 1), frames.append)
        self.assertNotEqual(frames[-1], b'\0'*4)
        self.assertFalse(model.cache)
        with self.assertRaises(InterruptedError):
            model.stream(('test', 'v', 'zh', 1), frames.append, lambda: True)
        self.assertFalse(model.cache)

    def test_stream_rejects_format_changes_and_nonfinite_audio(self):
        for samples, rate in [([float('nan')], 24000), ([0.1], 96000), ([[0.1,0.2,0.3]], 24000)]:
            def generate(*args):
                args[-2](samples, rate)
            with self.assertRaises(ValueError):
                StreamingSynthesizer(generate).stream(('t','v','zh',1), lambda _: None)
        def changed(*args):
            args[-2]([0.1], 24000)
            args[-2]([0.1], 48000)
        with self.assertRaises(ValueError):
            StreamingSynthesizer(changed).stream(('t','v','zh',1), lambda _: None)

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
