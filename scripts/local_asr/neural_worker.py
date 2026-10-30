# /// script
# requires-python = ">=3.12,<3.13"
# dependencies = ["torch==2.9.1", "safetensors==0.8.0"]
# ///
"""Faithful Torch adapter for qingjian-neural CharLm, P2c and CharScorer.
GPL-3.0-or-later upstream tensor/architecture contract, no remote-code/model fetching.
"""
from __future__ import annotations
import http.server
import json
import math
import os
from pathlib import Path
import secrets
import sys
import time
import torch
from torch import nn
from torch.nn import functional as F
from safetensors.torch import load as load_safetensors
import manager as support
from neural_qjm import read_qjm

class Attention(nn.Module):
    def __init__(self,n):
        super().__init__();self.qkv=nn.Linear(n,3*n);self.proj=nn.Linear(n,n)
class Mlp(nn.Module):
    def __init__(self,n):
        super().__init__();self.fc=nn.Linear(n,4*n);self.proj=nn.Linear(4*n,n)
class Block(nn.Module):
    def __init__(self,n):
        super().__init__();self.ln1=nn.LayerNorm(n,eps=1e-5);self.attn=Attention(n);self.ln2=nn.LayerNorm(n,eps=1e-5);self.mlp=Mlp(n)
class CharLm(nn.Module):
    def __init__(self,cfg):
        super().__init__();self.cfg=cfg;n=cfg['n_embd'];self.tok_emb=nn.Embedding(cfg['vocab_size'],n);self.pos_emb=nn.Embedding(cfg['context'],n);self.blocks=nn.ModuleList([Block(n) for _ in range(cfg['n_layer'])]);self.ln_f=nn.LayerNorm(n,eps=1e-5)
    def forward(self,idx,past=None):
        b,t=idx.shape;offset=0 if past is None else past[0][0].shape[2]
        if offset+t>self.cfg['context']:raise ValueError('输入超出模型上下文')
        x=self.tok_emb(idx)+self.pos_emb.weight[offset:offset+t];cache=[];h=self.cfg['n_head'];d=self.cfg['n_embd']//h
        allowed=torch.arange(offset+t,device=x.device)[None,:]<=offset+torch.arange(t,device=x.device)[:,None]
        for i,block in enumerate(self.blocks):
            q,k,v=block.attn.qkv(block.ln1(x)).chunk(3,dim=-1)
            q,k,v=[a.view(b,t,h,d).transpose(1,2) for a in (q,k,v)]
            if past is not None:
                pk,pv=past[i];k=torch.cat((pk.expand(b,-1,-1,-1),k),dim=2);v=torch.cat((pv.expand(b,-1,-1,-1),v),dim=2)
            att=(q@k.transpose(-2,-1))/math.sqrt(d);att=att.masked_fill(~allowed,-1e4).softmax(-1)
            x=x+block.attn.proj((att@v).transpose(1,2).contiguous().view(b,t,-1));x=x+block.mlp.proj(F.gelu(block.mlp.fc(block.ln2(x)),approximate='none'));cache.append((k,v))
        return F.linear(self.ln_f(x),self.tok_emb.weight),cache

class Adapter:
    def __init__(self,loaded,device='cpu'):
        self.tokens=loaded['tokens'];self.index={t:i for i,t in enumerate(self.tokens) if len(t)==1};self.sep=self.tokens.index('<sep>') if '<sep>' in self.tokens else None;self.capability=loaded['capability'];self.cfg=loaded['config'];self.device=torch.device(device)
        self.model=CharLm(self.cfg).to(self.device);weights=load_safetensors(loaded['weights']);self.model.load_state_dict(weights,strict=True);self.model.eval()
    def encode(self,text):return [self.index.get(c,1) for c in text]
    def tensor(self,ids):return torch.tensor(ids,dtype=torch.long,device=self.device)
    @torch.inference_mode()
    def convert(self,keys,beam=3,max_chars=48,cancelled=lambda:False):
        if self.sep is None:raise ValueError('知微只支持已有候选重排，不能拼音生成')
        prefix=[2]+self.encode(keys)+[self.sep];beam=max(1,min(10,beam));max_chars=min(max_chars,self.cfg['context']-len(prefix))
        if max_chars<1:raise ValueError('拼音太长，超出模型上下文')
        logits,cache=self.model(self.tensor([prefix]));lp=logits[:,-1].float().log_softmax(-1);beams=[(0.,[])];done=[]
        for _ in range(max_chars):
            if cancelled():raise InterruptedError('任务已取消')
            vals,ids=lp.topk(min(beam,len(self.tokens)),dim=-1);pool=[]
            for i,(score,seq) in enumerate(beams):
                pool.extend((score+float(v),i,int(tok)) for v,tok in zip(vals[i].tolist(),ids[i].tolist()))
            pool.sort(key=lambda x:x[0],reverse=True);nxt=[];order=[];token_ids=[]
            for score,i,tok in pool:
                if tok==2:done.append((score,beams[i][1]));continue
                if len(nxt)>=beam:continue
                nxt.append((score,beams[i][1]+[tok]));order.append(i);token_ids.append([tok])
            if not nxt or len(done)>=beam:beams=nxt;break
            selected=self.tensor(order);cache=[(k.index_select(0,selected),v.index_select(0,selected)) for k,v in cache];logits,cache=self.model(self.tensor(token_ids),cache);lp=logits[:,-1].float().log_softmax(-1);beams=nxt
        done+=beams;done.sort(key=lambda x:x[0],reverse=True);out=[];seen=set()
        for _,seq in done:
            # Reject rather than print special token artifacts as candidate text.
            if any(len(self.tokens[i])!=1 for i in seq):continue
            text=''.join(self.tokens[i] for i in seq)
            if text and text not in seen:out.append({'text':text,'pinyin':''});seen.add(text)
            if len(out)>=beam:break
        if not out:raise ValueError('模型没有生成有效汉字候选')
        return out
    @torch.inference_mode()
    def rerank(self,context,rows,cancelled=lambda:False):
        if self.capability!='rerank':raise ValueError('此模型不是知微重排模型')
        if not rows:raise ValueError('知微只重排已有整句候选；请先输入拼音取得原候选')
        scored=[];limit=self.cfg['context']
        for row in rows[:32]:
            if cancelled():raise InterruptedError('任务已取消')
            text=row.get('text','')
            if not isinstance(text,str) or not text or len(text)>128:continue
            tail=self.encode(text)
            if len(tail)>limit-1:continue
            head=[2]+self.encode(context);head=head[-(limit-len(tail)):];seq=head+tail
            logits,_=self.model(self.tensor([seq]));lp=logits[0].float().log_softmax(-1);start=len(head)-1;target=self.tensor(tail);score=float(lp[start:start+len(tail)].gather(1,target[:,None]).sum());scored.append((score,{'text':text,'pinyin':row.get('pinyin','')}))
        scored.sort(key=lambda x:x[0],reverse=True)
        if not scored:raise ValueError('没有适合知微重排的完整候选')
        return [r for _,r in scored]
    def run(self,input,cancelled=lambda:False):
        if input.get('task')!='pinyin':raise ValueError('含章模型只支持拼音生成/候选重排，不支持问字或续写')
        if self.capability=='rerank':rows=self.rerank(input.get('context',''),input.get('original_candidates',[]),cancelled)
        else:
            segments=input.get('segmented_pinyin',[])
            if not isinstance(segments,list) or not segments or any(not isinstance(x,str) for x in segments):raise ValueError('拼音分段无效')
            keys=''.join(segments).lower().replace("'",'').replace(' ','')
            if not keys.isascii() or not keys.isalpha() or len(keys)>128:raise ValueError('含章·通变需要 1–128 个拼音字母')
            rows=self.convert(keys,input.get('candidate_limit',3),cancelled=cancelled)
        return {'candidates':rows[:max(1,min(10,int(input.get('candidate_limit',3))))]}

def serve(root,model_id):
    from neural_manager import model_path
    root=Path(root);stop=root/'stop';token=secrets.token_urlsafe(32);(root/'service-token.txt').write_text(token,encoding='utf8')
    try:
        loaded=read_qjm(model_path(root,model_id),True,model_id);torch.set_num_threads(4);device='cuda:0' if torch.cuda.is_available() else 'cpu'
        try:adapter=Adapter(loaded,device)
        except RuntimeError:
            if device=='cpu':raise
            torch.cuda.empty_cache();device='cpu';adapter=Adapter(loaded,device)
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def reply(self,status,value):
                data=json.dumps(value,ensure_ascii=False).encode('utf8');self.send_response(status);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
            def auth(self):return secrets.compare_digest(self.headers.get('Authorization',''),'Bearer '+token)
            def do_GET(self):
                if not self.auth():return self.reply(401,{'error':'unauthorized'})
                if self.path!='/v1/models':return self.reply(404,{'error':'unsupported endpoint'})
                self.reply(200,{'data':[{'id':model_id}]})
            def do_POST(self):
                if not self.auth():return self.reply(401,{'error':'unauthorized'})
                if self.path!='/v1/chat/completions':return self.reply(404,{'error':'unsupported endpoint'})
                try:
                    self.connection.settimeout(10);size=int(self.headers.get('Content-Length','0'))
                    if not 0<size<=64*1024:raise ValueError('请求过长')
                    body=self.rfile.read(size)
                    if len(body)!=size:raise ValueError('请求不完整')
                    req=json.loads(body)
                    if req.get('model')!=model_id:raise ValueError('所选模型与本地服务不一致')
                    messages=req.get('messages',[]);user=next((x for x in reversed(messages) if x.get('role')=='user'),None)
                    if not user:raise ValueError('任务缺少输入')
                    result=adapter.run(json.loads(user['content']),stop.exists)
                    self.reply(200,{'capability':adapter.capability,'choices':[{'message':{'content':json.dumps(result,ensure_ascii=False)}}]})
                except Exception as error:self.reply(400,{'error':str(error)[:240]})
        server=http.server.HTTPServer(('127.0.0.1',18769),Handler);server.timeout=.5
        support.atomic_json(root/'service.json',{'state':'running','model':model_id,'pid':os.getpid(),'device':device,'message':'含章本地整句已就绪 · '+device,'endpoint':'http://127.0.0.1:18769/v1/chat/completions'})
        try:
            while not stop.exists():server.handle_request()
        finally:server.server_close()
        support.atomic_json(root/'service.json',{'state':'stopped','model':model_id,'message':'已停止'})
    except Exception as error:
        support.atomic_json(root/'service.json',{'state':'error','model':model_id,'message':str(error)[:300]})
        raise
    finally:(root/'service-token.txt').unlink(missing_ok=True)

if __name__=='__main__':serve(Path(sys.argv[1]),sys.argv[2])
