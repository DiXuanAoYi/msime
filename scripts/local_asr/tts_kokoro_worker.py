# /// script
# requires-python = ">=3.12,<3.13"
# dependencies = ["numpy==2.3.5", "sherpa-onnx==1.13.8"]
# ///
from pathlib import Path
import sys
from tts_manager import serve
if __name__=='__main__':serve(Path(sys.argv[1]),sys.argv[2],externally_locked=True)
