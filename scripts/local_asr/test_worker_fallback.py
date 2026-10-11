import json,os,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
import manager

class WorkerFallbackTests(unittest.TestCase):
 def test_native_gpu_crash_retries_cpu_with_same_root_and_model(self):
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp);calls=[]
   class Child:
    pid=123
    def __init__(self,command,**kwargs):
     calls.append((list(command),dict(kwargs['env'])));self.returncode=3 if len(calls)==1 else 0
    def poll(self):return self.returncode
   with patch.object(manager.subprocess,'Popen',Child),patch.dict(os.environ,{'MSIME_TTS_OPERATION':'test-op'}):manager.run_qwen(root,'kokoro','tts_kokoro_cuda_worker.py')
   self.assertEqual(len(calls),2)
   self.assertTrue(calls[1][0][6].endswith('tts_kokoro_worker.py'))
   self.assertEqual(calls[1][0][-2:],[str(root),'kokoro'])
   self.assertEqual(calls[1][1]['MSIME_GPU_FALLBACK'],'1')
   self.assertEqual(calls[1][1]['MSIME_TTS_OPERATION'],'test-op')
   self.assertEqual(calls[1][1]['CUDA_VISIBLE_DEVICES'],'')
 def test_stop_does_not_start_a_fallback(self):
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp);calls=[]
   class Child:
    pid=123;returncode=3
    def __init__(self,command,**kwargs):calls.append(command);(root/'stop').write_text('stop')
    def poll(self):return 3
   with patch.object(manager.subprocess,'Popen',Child):manager.run_qwen(root,'kokoro','tts_kokoro_cuda_worker.py')
   self.assertEqual(len(calls),1)
if __name__=='__main__':unittest.main()
