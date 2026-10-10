import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import wave
import manager


class LocalAsrTests(unittest.TestCase):
    def test_short_response_preserves_partial_and_resumes(self):
        import hashlib
        payload = b'complete model'
        entry = dict(id='fixture', engine='qwen', downloadBytes=len(payload), diskBytes=100,
                     files=[dict(name='model.bin', bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest(), url='https://example.com/model')])
        def response(data, status, headers):
            stream = io.BytesIO(data)
            stream.status = status
            stream.headers = headers
            return stream
        with tempfile.TemporaryDirectory() as tmp, patch.object(manager, 'CATALOG', [entry]):
            root = Path(tmp)
            with patch.object(manager.urllib.request, 'urlopen', return_value=response(payload[:4], 200, {})):
                manager.download(root, 'fixture')
            self.assertEqual(json.loads((root/'fixture/state.json').read_text())['state'], 'paused')
            self.assertEqual((root/'fixture/original/model.bin.part').read_bytes(), payload[:4])
            with patch.object(manager.urllib.request, 'urlopen', return_value=response(payload[4:], 206, {'Content-Range': f'bytes 4-{len(payload)-1}/{len(payload)}'})) as request:
                manager.download(root, 'fixture')
                self.assertEqual(request.call_args.args[0].get_header('Range'), 'bytes=4-')
            self.assertEqual((root/'fixture/original/model.bin').read_bytes(), payload)
            self.assertEqual(json.loads((root/'fixture/state.json').read_text())['state'], 'ready')

    def test_atomic_status_retries_windows_reader_contention(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / 'state.json'
            replace = manager.os.replace
            calls = []
            def busy_once(source, dest):
                calls.append(dest)
                if len(calls) == 1:
                    raise PermissionError('Windows reader temporarily holds file')
                replace(source, dest)
            with patch.object(manager.os, 'replace', side_effect=busy_once):
                manager.atomic_json(target, {'downloaded': 123, 'total': 456})
            self.assertEqual(len(calls), 2)
            self.assertEqual(json.loads(target.read_text())['downloaded'], 123)

    def test_catalog_sizes_and_pinned_integrity(self):
        self.assertEqual(len(manager.CATALOG), 5)
        for model in manager.CATALOG:
            self.assertEqual(model['downloadBytes'], sum(f['bytes'] for f in model['files']))
            for file in model['files']:
                self.assertRegex(file['sha256'], r'^[0-9a-f]{64}$')
                self.assertTrue(file['url'].startswith('https://'))
                self.assertNotIn('/resolve/main/', file['url'])
        small = manager.model_entry('sensevoice-small')
        self.assertEqual(small['downloadBytes'], 163002883)
        self.assertLess(small['downloadBytes'], small['originalBytes'])

    def test_archive_cannot_escape_destination_or_create_links(self):
        for name, type_ in [('../outside', tarfile.REGTYPE), ('absolute/link', tarfile.SYMTYPE), ('C:/outside', tarfile.REGTYPE)]:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                archive = Path(tmp) / 'model.tar.bz2'
                with tarfile.open(archive, 'w:bz2') as tar:
                    member = tarfile.TarInfo(name); member.type = type_; member.linkname = '../outside'
                    member.size = 1 if type_ == tarfile.REGTYPE else 0
                    tar.addfile(member, io.BytesIO(b'x'))
                with self.assertRaises(ValueError):
                    manager.safe_extract(archive, Path(tmp) / 'safe', 1024)
                self.assertFalse((Path(tmp) / 'outside').exists())

    def test_archive_size_limit(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / 'model.tar.bz2'
            with tarfile.open(archive, 'w:bz2') as tar:
                member = tarfile.TarInfo('model/weights'); member.size = 128
                tar.addfile(member, io.BytesIO(b'0' * 128))
            with self.assertRaises(ValueError):
                manager.safe_extract(archive, Path(tmp) / 'safe', 64)

    def test_upload_rejects_wrong_model_and_non_pcm_format(self):
        def upload(rate, model):
            data = io.BytesIO()
            with wave.open(data, 'wb') as wav:
                wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(rate); wav.writeframes(b'\0' * 320)
            boundary = 'MSIMEtest'
            body = (f'--{boundary}\r\nContent-Disposition: form-data; name="model"\r\n\r\n{model}\r\n'
                    f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="x.wav"\r\nContent-Type: audio/wav\r\n\r\n').encode() + data.getvalue() + f'\r\n--{boundary}--\r\n'.encode()
            return manager.decode_upload(body, 'multipart/form-data; boundary=' + boundary, 'sensevoice-small')
        self.assertEqual(len(upload(16000, 'sensevoice-small')), 160)
        with self.assertRaises(ValueError): upload(48000, 'sensevoice-small')
        with self.assertRaises(ValueError): upload(16000, 'another-model')


if __name__ == '__main__':
    unittest.main()
