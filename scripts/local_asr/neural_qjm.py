"""Strict, bounded reader for the official QINGJIAN v1 model container.
Source contract: qingjian-format and qingjian-neural, GPL-3.0-or-later (dd9c471).
No torch imports, network access, pickle, or automatic model downloads.
"""
from __future__ import annotations
import json
import math
from pathlib import Path
import struct
import tomllib

MAX_MODEL_BYTES = 1024 * 1024 * 1024
DTYPES = {'F32':4,'F16':2,'BF16':2}

def expected_shapes(cfg):
    v,n,c,l = cfg['vocab_size'], cfg['n_embd'], cfg['context'], cfg['n_layer']
    shapes={'tok_emb.weight':[v,n],'pos_emb.weight':[c,n],'ln_f.weight':[n],'ln_f.bias':[n]}
    for i in range(l):
        b=f'blocks.{i}.'
        for prefix in ('ln1','ln2'):shapes[b+prefix+'.weight']=[n];shapes[b+prefix+'.bias']=[n]
        for name,shape in [('attn.qkv',[3*n,n]),('attn.proj',[n,n]),('mlp.fc',[4*n,n]),('mlp.proj',[n,4*n])]:
            shapes[b+name+'.weight']=shape;shapes[b+name+'.bias']=[shape[0]]
    return shapes

def read_qjm(path, include_weights=False, expected=None):
    path=Path(path);size=path.stat().st_size
    if not 128 <= size <= MAX_MODEL_BYTES:raise ValueError('QJM 文件大小无效（最大 1 GiB）')
    with path.open('rb') as f:
        h=f.read(32)
        magic,version,kind,count,reserved=struct.unpack('<8sHHI16s',h)
        if magic!=b'QINGJIAN' or version!=1 or kind!=6 or reserved!=bytes(16) or not 4<=count<=32:
            raise ValueError('不是支持的 QINGJIAN v1 模型容器')
        table=f.read(count*24);sections={};bounds=[]
        for i in range(count):
            tag,pad,offset,length=struct.unpack_from('<4sIQQ',table,i*24)
            if pad or tag in sections or offset%8 or offset<32+count*24 or offset+length>size:
                raise ValueError('QJM 分节边界、对齐或标签无效')
            sections[tag]=(offset,length);bounds.append((offset,offset+length))
        if any(b[0]<a[1] for a,b in zip(sorted(bounds),sorted(bounds)[1:])):raise ValueError('QJM 分节重叠')
        def read(tag,limit):
            if tag not in sections:raise ValueError('QJM 缺少必需分节')
            off,length=sections[tag]
            if length>limit:raise ValueError('QJM 元数据或字表过大')
            f.seek(off);body=f.read(length)
            if len(body)!=length:raise ValueError('QJM 文件不完整')
            return body
        meta=tomllib.loads(read(b'META',256*1024).decode('utf8'))
        cfg=json.loads(read(b'CONF',256*1024))
        limits={'vocab_size':65536,'n_layer':64,'n_embd':2048,'n_head':128,'context':4096}
        if any(type(cfg.get(k)) is not int or not 1<=cfg[k]<=lim for k,lim in limits.items()) or cfg['n_embd']%cfg['n_head']:
            raise ValueError('QJM 模型结构参数无效或超出支持范围')
        tokens=json.loads(read(b'VOCB',8*1024*1024)).get('tokens')
        if not isinstance(tokens,list) or len(tokens)!=cfg['vocab_size'] or len(tokens)<4 or any(not isinstance(t,str) or not t or len(t)>64 for t in tokens) or len(set(tokens))!=len(tokens):
            raise ValueError('QJM 字表与结构不匹配')
        # IDs 0/1/2 are the official pad/unknown/eos training contract.
        if tokens[2]!='<eos>':raise ValueError('QJM 句首/结束 token 不兼容')
        is_p2c='<sep>' in tokens
        if expected=='tongbian' and not is_p2c:raise ValueError('含章·通变必须有 <sep> 拼音分隔 token')
        if expected=='zhiwei' and is_p2c:raise ValueError('请选择知微字级重排模型，不能混用通变')
        if b'SAFT' not in sections:raise ValueError('QJM 缺少 safetensors 权重')
        off,length=sections[b'SAFT'];f.seek(off)
        if length<8:raise ValueError('权重分节无效')
        header_size=struct.unpack('<Q',f.read(8))[0]
        if not 2<=header_size<=1024*1024 or header_size+8>length:raise ValueError('权重头大小无效')
        header=json.loads(f.read(header_size));tensors={k:v for k,v in header.items() if k!='__metadata__'}
        shapes=expected_shapes(cfg)
        if set(tensors)!=set(shapes):raise ValueError('QJM 张量名称与含章 Transformer 不一致')
        ranges=[];parameters=0
        for name,shape in shapes.items():
            row=tensors[name]
            if not isinstance(row,dict) or row.get('shape')!=shape or row.get('dtype') not in DTYPES:raise ValueError('QJM 张量形状或精度不匹配')
            offsets=row.get('data_offsets');n=math.prod(shape);parameters+=n
            if not isinstance(offsets,list) or len(offsets)!=2 or any(type(x) is not int for x in offsets) or offsets[0]<0 or offsets[1]-offsets[0]!=n*DTYPES[row['dtype']] or offsets[1]>length-8-header_size:
                raise ValueError('QJM 张量数据边界无效')
            ranges.append(tuple(offsets))
        ranges.sort()
        if not ranges or ranges[0][0]!=0 or ranges[-1][1]!=length-8-header_size or any(a[1]!=b[0] for a,b in zip(ranges,ranges[1:])):
            raise ValueError('QJM 张量数据不连续或有重叠')
        body=None
        if include_weights:
            f.seek(off);body=f.read(length)
        return {'config':cfg,'tokens':tokens,'metadata':meta,'tensors':tensors,'header_size':header_size,
                'weights':body,'bytes':size,'parameters':parameters,'capability':'p2c' if is_p2c else 'rerank'}
