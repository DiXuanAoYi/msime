"""Same explicit download, cancellation and isolated-worker lifecycle as ASR/TTS."""
import argparse,json,os
from pathlib import Path
import manager as support
from tts_manager import switch_service,nvidia_available

CATALOG=json.loads((Path(__file__).parent/'ocr_catalog.json').read_text(encoding='utf8'))
def main():
    p=argparse.ArgumentParser();p.add_argument('--ocr',action='store_true');p.add_argument('--root',type=Path,required=True);p.add_argument('--id',required=True);p.add_argument('--action',choices=['download','serve','switch'],required=True);p.add_argument('--operation',default='');args=p.parse_args()
    if args.id not in {x['id'] for x in CATALOG}:raise ValueError('未知 OCR 模型')
    args.root.mkdir(parents=True,exist_ok=True);support.CATALOG=CATALOG
    if args.action=='download':support.download(args.root,args.id);return
    worker='ocr_paddle_worker.py' if args.id=='pp-ocrv6' else 'ocr_vlm_worker.py'
    if worker=='ocr_vlm_worker.py' and not nvidia_available():worker='ocr_vlm_cpu_worker.py'
    def run(guard=lambda:True):support.run_qwen(args.root,args.id,worker_name=worker,guard=guard)
    if args.action=='switch':
        try:switch_service(args.root,args.id,args.operation,run)
        except Exception as error:support.atomic_json(args.root/'switch-error.json',dict(operation=args.operation,message=str(error)[:240]))
    else:run()
