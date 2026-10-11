# /// script
# requires-python = ">=3.12,<3.13"
# dependencies = ["paddlepaddle==3.2.2", "paddleocr==3.7.0", "pillow==12.1.0"]
# ///
from pathlib import Path
import sys
from ocr_runtime import serve,load_paddle
if __name__=='__main__':serve(Path(sys.argv[1]),sys.argv[2],load_paddle)
