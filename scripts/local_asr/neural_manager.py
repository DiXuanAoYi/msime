"""Explicit-only neural model management. Status never downloads/loads models."""
from __future__ import annotations
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request
import manager as support
from neural_qjm import read_qjm, MAX_MODEL_BYTES

BUNDLE_URL='https://github.com/qingjian-team/qingjian/releases/download/data-v6/qingjian-data.tar.gz'
BUNDLE_BYTES=143814324
BUNDLE_SHA='98bdf7a8d4c65cf913e78c9209255ce9bf03785f0ef6f83e1a4e1bfef85e255e'
CATALOG=[
 {'id':'desktop','name':'水杉神经整句 · 效果优先','filename':'sentence-model-desktop.safetensors','bytes':25480184,'sha256':'0a6ecba69bf1d39c7eb49549c716477dd6c03fdf05757773e1f50435262fb469','url':'https://github.com/metasequoiaime/chinese-ime-lm/releases/download/model-v1/sentence-model-desktop.safetensors','license':'Apache-2.0','capability':'sentence','note':'约 24.30 MiB；沿用原本地整句引擎，不启动 Python 服务'},
 {'id':'tongbian','name':'含章·通变 · 拼音生成','filename':'hanzhang-tongbian-small.qjm','bytes':None,'downloadBytes':BUNDLE_BYTES,'sha256':BUNDLE_SHA,'url':BUNDLE_URL,'license':'GPL-3.0-or-later','capability':'p2c','note':'官方 data-v6 资料包约 137.15 MiB（含其它数据），只提取所选模型；模型实际大小导入后显示。支持拼音生成，不支持聊天/问字'},
 {'id':'zhiwei','name':'含章·知微 · 候选重排','filename':'hanzhang-zhiwei-small.qjm','bytes':None,'downloadBytes':BUNDLE_BYTES,'sha256':BUNDLE_SHA,'url':BUNDLE_URL,'license':'GPL-3.0-or-later','capability':'rerank','note':'同一官方资料包约 137.15 MiB，只提取所选模型；仅对已有完整候选重排，不生成问答或续写'}]

def entry(model_id):
    try:return next(x for x in CATALOG if x['id']==model_id)
    except StopIteration:raise ValueError('未知本地整句模型') from None

def model_path(root,model_id):return Path(root)/model_id/entry(model_id)['filename']

def verify(path,model_id):
    info=entry(model_id);path=Path(path)
    if path.is_symlink() or not path.is_file():raise ValueError('请选择普通模型文件')
    if model_id=='desktop':
        if path.stat().st_size!=info['bytes'] or support.digest(path)!=info['sha256']:raise ValueError('效果优先模型大小或 SHA256 不符')
        return {'bytes':info['bytes'],'capability':'sentence'}
    return read_qjm(path,expected=model_id)

def install(root,model_id,source):
    root=Path(root);dest=model_path(root,model_id);dest.parent.mkdir(parents=True,exist_ok=True)
    part=dest.with_suffix(dest.suffix+'.part');source=Path(source)
    if source.stat().st_size>MAX_MODEL_BYTES:raise ValueError('模型超过 1 GiB 上限')
    shutil.copyfile(source,part)
    try:
        checked=verify(part,model_id)
        # Preserve a different previous managed model. Never touch legacy data-root files.
        if dest.exists() and support.digest(dest)!=support.digest(part):
            backup=dest.with_name(dest.name+f'.{time.time_ns()}.bak');os.replace(dest,backup)
        os.replace(part,dest)
        metadata={'state':'ready','bytes':checked['bytes'],'sha256':support.digest(dest),'capability':checked['capability'],'version':'model-v1' if model_id=='desktop' else 'data-v6','source':entry(model_id)['url'],'license':entry(model_id)['license'],'message':'模型已校验；原文件与旧版本保留'}
        support.atomic_json(dest.parent/'state.json',metadata)
    finally:part.unlink(missing_ok=True)

def download(root,model_id):
    root=Path(root);info=entry(model_id);dest=model_path(root,model_id);dest.parent.mkdir(parents=True,exist_ok=True)
    state=dest.parent/'state.json';cancel=dest.parent/'cancel';cancel.unlink(missing_ok=True)
    with support.job_lock(root/'download.lock'):
        archive=dest.parent/'download.part';expected=info.get('downloadBytes') or info['bytes'];received=0
        try:
            request=urllib.request.Request(info['url'],headers={'User-Agent':'MetasequoiaIME-neural/1'})
            with urllib.request.urlopen(request,timeout=30) as response,archive.open('wb') as out:
                if response.status!=200:raise ValueError('模型下载 HTTP 状态异常')
                length=response.headers.get('Content-Length')
                if length is not None and int(length)!=expected:raise ValueError('下载大小与冻结官方版本不符')
                while data:=response.read(1024*1024):
                    if cancel.exists():raise InterruptedError('已取消下载')
                    received+=len(data)
                    if received>expected:raise ValueError('模型下载超出预计大小')
                    out.write(data);support.atomic_json(state,{'state':'downloading','downloaded':received,'total':expected,'message':'仅应用户点击下载，不安装其它模型'})
            if received!=expected or support.digest(archive)!=info['sha256']:raise ValueError('下载大小或 SHA256 不符，未替换原模型')
            if model_id=='desktop':install(root,model_id,archive)
            else:
                # No generic extraction: a selected regular member copied to a fixed path only.
                extracted=dest.parent/'extracted.part';found=[]
                with tarfile.open(archive,'r:gz') as tar:
                    for m in tar:
                        name=Path(m.name)
                        if name.name!=info['filename']:continue
                        if not m.isfile() or m.size>MAX_MODEL_BYTES or '..' in name.parts:raise ValueError('官方包中的模型条目无效')
                        found.append(m)
                        if len(found)>1:raise ValueError('资料包有重复同名模型')
                        with tar.extractfile(m) as src,extracted.open('wb') as out:shutil.copyfileobj(src,out,1024*1024)
                if not found:raise ValueError('冻结资料包未包含所选模型；请手动导入官方 QJM')
                try:install(root,model_id,extracted)
                finally:extracted.unlink(missing_ok=True)
        except Exception as e:
            support.atomic_json(state,{'state':'error','message':str(e)[:300]})
        finally:archive.unlink(missing_ok=True)

def stop(root):
    root=Path(root);(root/'stop').touch()

def serve(root,model_id):
    if model_id=='desktop':raise ValueError('效果优先模型由输入引擎使用，无独立本地服务')
    verify(model_path(root,model_id),model_id)
    # Same owned worker lifecycle as existing ASR/TTS. Only run after explicit Start.
    support.run_qwen(Path(root),model_id,'neural_worker.py')

def switch(root,model_id):
    root=Path(root)
    with support.job_lock(root/'switch.lock'):
        stop(root)
        deadline=time.monotonic()+20
        while True:
            try:
                with support.job_lock(root/'service.lock'):pass
                break
            except OSError:
                if time.monotonic()>=deadline:raise TimeoutError('旧本地模型停止超时，请稍后重试')
                time.sleep(.1)
        serve(root,model_id)

def main():
    p=argparse.ArgumentParser();p.add_argument('--neural',action='store_true');p.add_argument('--root',type=Path,required=True);p.add_argument('--action',choices=['download','import','serve','switch'],required=True);p.add_argument('--id',required=True);p.add_argument('--source',type=Path)
    args=p.parse_args();entry(args.id);args.root.mkdir(parents=True,exist_ok=True)
    try:
        if args.action=='download':download(args.root,args.id)
        elif args.action=='import':
            with support.job_lock(args.root/'download.lock'):
                install(args.root,args.id,args.source)
        elif args.action=='switch':switch(args.root,args.id)
        else:serve(args.root,args.id)
    except Exception as e:
        target=args.root/'service.json' if args.action in ('serve','switch') else model_path(args.root,args.id).parent/'state.json'
        support.atomic_json(target,{'state':'error','model':args.id,'message':str(e)[:300]})

if __name__=='__main__':main()
