import io
import json
import unittest
import wave
import tts_manager as tts

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

if __name__=='__main__':
    unittest.main()
