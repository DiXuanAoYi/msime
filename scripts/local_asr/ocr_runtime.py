"""Authenticated loopback OCR runtime. Only explicitly downloaded local models."""
import base64
import contextlib
import hmac
import http.server
import io
import json
import os
from pathlib import Path
import secrets
import select
import socket
import sys
import time
from ocr_layout import MAX_PIXELS, MAX_OUTPUT_BYTES, combine_regions, deduplicate_spotted_blocks, image_tiles, normalize_box, offset_box, owns_spotted_box, rectangle_box, task_prompt

TASK_PROMPTS = {'text':'Text Recognition:', 'table':'Table Recognition:', 'formula':'Formula Recognition:',
                'code':'Recognize the code in the image. Preserve indentation, punctuation and line breaks. Output only the code.',
                'document':'Recognize this document. Preserve reading order, headings, tables as HTML, formulas as LaTeX, and code indentation. Output Markdown without commentary.'}

def decode_image(encoded):
    from PIL import Image,ImageOps
    if not isinstance(encoded,str) or len(encoded)>34*1024*1024:
        raise ValueError('图像限 25 MB')
    if encoded.startswith('data:'):
        encoded=encoded.split(',',1)[1]
    data=base64.b64decode(encoded,validate=True)
    if len(data)>25*1024*1024:raise ValueError('图像限 25 MB')
    image=Image.open(io.BytesIO(data))
    if image.width<1 or image.height<1 or image.width*image.height>MAX_PIXELS:raise ValueError('图片超过 6400 万像素，请分区识别')
    image.load()
    return ImageOps.exif_transpose(image).convert('RGB')

def load_vlm(root,model_id):
    os.environ['HF_HUB_OFFLINE']='1'
    os.environ['TRANSFORMERS_OFFLINE']='1'
    import torch
    from transformers import AutoProcessor, AutoModelForImageTextToText, StoppingCriteria, StoppingCriteriaList
    folder=root/model_id/'original'
    if not (root/model_id/'ready.json').is_file():raise ValueError('请先下载并校验模型，启动不会自动下载权重')
    processor=AutoProcessor.from_pretrained(str(folder),local_files_only=True,trust_remote_code=False)
    device='cuda:0' if torch.cuda.is_available() else 'cpu'
    def create(where):
        return AutoModelForImageTextToText.from_pretrained(str(folder),local_files_only=True,trust_remote_code=False,
                     torch_dtype=torch.bfloat16 if where.startswith('cuda') else torch.float32,attn_implementation='sdpa').to(where).eval()
    try:model=create(device)
    except (RuntimeError,MemoryError):
        if device=='cpu':raise
        import gc
        gc.collect();torch.cuda.empty_cache();device='cpu';model=create(device)
    class Stop(StoppingCriteria):
        def __init__(self,cancelled):self.cancelled=cancelled
        def __call__(self,*args,**kwargs):return (root/'stop').exists() or self.cancelled()
    def recognize(image,task,max_tokens,cancelled=lambda:False):
        prompt=task_prompt(model_id,task)
        def regions():
            for tile,bounds,row,column in image_tiles(image):
                if (root/'stop').exists() or cancelled():raise InterruptedError('已停止识别')
                messages=[{'role':'user','content':[{'type':'image','image':tile},{'type':'text','text':prompt}]}]
                try:
                    inputs=processor.apply_chat_template(messages,tokenize=True,add_generation_prompt=True,return_dict=True,return_tensors='pt')
                except (TypeError,ValueError):
                    text=processor.apply_chat_template(messages,tokenize=False,add_generation_prompt=True)
                    inputs=processor(text=[text],images=[tile],return_tensors='pt')
                inputs=inputs.to(device)
                with torch.inference_mode():
                    output=model.generate(**inputs,max_new_tokens=max_tokens,do_sample=False,stopping_criteria=StoppingCriteriaList([Stop(cancelled)]))
                if (root/'stop').exists() or cancelled():raise InterruptedError('已停止识别')
                generated=output[:,inputs['input_ids'].shape[-1]:]
                text=processor.batch_decode(generated,skip_special_tokens=True,clean_up_tokenization_spaces=False)[0]
                yield dict(text=text,bounds=bounds,row=row,column=column,truncated=generated.shape[-1]>=max_tokens)
                # Each crop can fill the GPU. Do not keep tensors from earlier crops.
                del generated,output,inputs
        result=combine_regions(regions(),task,model_id,image.width,image.height)
        result['device']=device
        return result
    return recognize,device

def load_paddle(root,model_id):
    import numpy as np
    from paddleocr import PaddleOCR
    folder=root/model_id/'original'
    if not (root/model_id/'ready.json').is_file():raise ValueError('请先下载 PP-OCRv6')
    pipeline=PaddleOCR(text_detection_model_name='PP-OCRv6_small_det',text_detection_model_dir=str(folder/'det'),
          text_recognition_model_name='PP-OCRv6_small_rec',text_recognition_model_dir=str(folder/'rec'),
          use_doc_orientation_classify=False,use_doc_unwarping=False,use_textline_orientation=False,device='cpu')
    def recognize(image,task,max_tokens,cancelled=lambda:False):
        if task in ('table','formula'):raise ValueError('PP-OCRv6 为文字识别，请用 GLM-OCR / PaddleOCR-VL 识别表格或公式结构')
        blocks=[];raw=[]
        for tile,bounds,row,column in image_tiles(image):
            if cancelled():raise InterruptedError('已停止识别')
            predicted=[]
            for result in pipeline.predict(np.asarray(tile)):
                if cancelled():raise InterruptedError('已停止识别')
                data=result.json
                if isinstance(data,str):data=json.loads(data)
                data=data.get('res',data)
                for text,score,poly in zip(data.get('rec_texts',[]),data.get('rec_scores',[]),data.get('rec_polys',[])):
                    original_box=np.asarray(poly).tolist()
                    predicted.append(dict(text=text,confidence=float(score),box=original_box))
                    box=normalize_box(original_box,tile.width,tile.height)
                    if not owns_spotted_box(box,bounds,image.width,image.height):continue
                    blocks.append(dict(text=text,confidence=float(score),box=offset_box(box,bounds[0],bounds[1],image.width,image.height),kind=task,format='text',region=False))
                    if len(blocks)>16000:raise ValueError('识别区域过多，请分段识别')
            raw.append(dict(box=rectangle_box(bounds),blocks=predicted))
        blocks=deduplicate_spotted_blocks(blocks)
        text='\n'.join(b['text'] for b in blocks)
        if len(text.encode('utf8'))>MAX_OUTPUT_BYTES:raise ValueError('识别文字超过 2 MB，请分段识别')
        return {'text':text,'blocks':blocks,'format':'text','device':'cpu','truncated':False,'width':image.width,'height':image.height,'raw':{'regions':raw},
                'warning':'PP-OCRv6 为文字检测识别；代码缩进、文档阅读顺序需对照原图核对。' if task in ('code','document') else ''}
    return recognize,'cpu'

def serve(root,model_id,loader=load_vlm):
    import manager
    root=Path(root);state=root/'service.json';stop=root/'stop';token=secrets.token_hex(32)
    try:
        manager.atomic_json(state,dict(state='loading',model=model_id,operation=os.environ.get('MSIME_TTS_OPERATION',''),pid=os.getpid(),message='正在加载已下载的 OCR 模型'))
        recognize,device=loader(root,model_id)
        (root/'service-token.txt').write_text(token,encoding='ascii')
        class Handler(http.server.BaseHTTPRequestHandler):
            timeout=15
            def log_message(self,*args):pass
            def reply(self,status,data):
                body=json.dumps(data,ensure_ascii=False).encode('utf8');self.send_response(status);self.send_header('Content-Type','application/json; charset=utf-8');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
            def do_POST(self):
                if self.path!='/v1/ocr':self.reply(404,{'error':'unknown endpoint'});return
                if self.headers.get('Origin') or not hmac.compare_digest(self.headers.get('Authorization',''),'Bearer '+token):self.reply(401,{'error':'unauthorized'});return
                try:
                    length=int(self.headers.get('Content-Length','0'))
                    if not 0<length<=35*1024*1024:raise ValueError('请求大小无效')
                    body=json.loads(self.rfile.read(length));task=body.get('task','text')
                    if task not in TASK_PROMPTS:raise ValueError('识别任务无效')
                    limit=body.get('maxTokens',4096)
                    if isinstance(limit,bool) or not isinstance(limit,int) or not 128<=limit<=8192:raise ValueError('输出长度限 128–8192')
                    def cancelled():
                        if stop.exists():return True
                        readable,_,_=select.select([self.connection],[],[],0)
                        return bool(readable) and not self.connection.recv(1,socket.MSG_PEEK)
                    start=time.perf_counter();result=recognize(decode_image(body.get('image','')),task,limit,cancelled);result['elapsedMs']=round((time.perf_counter()-start)*1000);self.reply(200,result)
                except (BrokenPipeError,ConnectionResetError):pass
                except Exception as e:self.reply(400,{'error':str(e)[:300]})
        server=http.server.HTTPServer(('127.0.0.1',18768),Handler);server.timeout=.5
        manager.atomic_json(state,dict(state='running',model=model_id,operation=os.environ.get('MSIME_TTS_OPERATION',''),pid=os.getpid(),message='OCR 已就绪 · '+device))
        try:
            while not stop.exists():server.handle_request()
        finally:server.server_close()
        manager.atomic_json(state,dict(state='stopped',model=model_id,message='已停止'))
    except Exception as e:manager.atomic_json(state,dict(state='error',model=model_id,operation=os.environ.get('MSIME_TTS_OPERATION',''),message=str(e)[:300]))
    finally:(root/'service-token.txt').unlink(missing_ok=True)
