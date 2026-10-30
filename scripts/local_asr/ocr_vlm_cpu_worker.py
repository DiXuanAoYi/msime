# /// script
# requires-python = ">=3.12,<3.13"
# dependencies = ["torch==2.9.1+cpu", "transformers==5.15.1", "accelerate==1.12.0", "pillow==12.1.0", "sentencepiece==0.2.1"]
# [tool.uv.sources]
# torch = { index = "pytorch" }
# [[tool.uv.index]]
# name = "pytorch"
# url = "https://download.pytorch.org/whl/cpu"
# explicit = true
# ///
from pathlib import Path
import sys
from ocr_runtime import serve
if __name__=='__main__':serve(Path(sys.argv[1]),sys.argv[2])
