# /// script
# requires-python = ">=3.12,<3.13"
# dependencies = ["torch==2.9.1+cu128", "transformers==4.57.3", "numpy==2.3.5", "scipy==1.16.3", "uroman==1.3.1"]
# [tool.uv.sources]
# torch = { index = "pytorch-cu128" }
# [[tool.uv.index]]
# name = "pytorch-cu128"
# url = "https://download.pytorch.org/whl/cu128"
# explicit = true
# ///
from pathlib import Path
import sys
from tts_mms_worker import load
from tts_manager import serve
if __name__=='__main__':serve(Path(sys.argv[1]),sys.argv[2],load,externally_locked=True)
