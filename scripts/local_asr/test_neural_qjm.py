"""Artificial owned QJM fixtures only; never fetch any actual model weights."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
from neural_qjm import read_qjm, expected_shapes


def pack(path,cfg,tokens,weights):
    sections=[(b'META',b'name = "Artificial unit fixture"\nlicense = "MIT"\nentries = 0\n'),(b'CONF',json.dumps(cfg).encode()),(b'VOCB',json.dumps({'tokens':tokens}).encode()),(b'SAFT',weights)]
    cursor=32+24*len(sections);body=bytearray(cursor);struct.pack_into('<8sHHI16s',body,0,b'QINGJIAN',1,6,len(sections),bytes(16))
    for i,(tag,data) in enumerate(sections):
        cursor=(cursor+7)//8*8
        while len(body)<cursor:body.append(0)
        struct.pack_into('<4sIQQ',body,32+i*24,tag,0,cursor,len(data));body.extend(data);cursor+=len(data)
    Path(path).write_bytes(body)


def fixture(path,p2c=True):
    cfg={'vocab_size':8,'n_layer':1,'n_embd':8,'n_head':2,'context':64};tokens=['<pad>','<unk>','<eos>','<sep>' if p2c else '<context>','n','i','你','好']
    header={};body=bytearray()
    for name,shape in expected_shapes(cfg).items():
        size=4
        for dim in shape:size*=dim
        header[name]={'shape':shape,'dtype':'F32','data_offsets':[len(body),len(body)+size]};body.extend(bytes(size))
    encoded=json.dumps(header).encode();pack(path,cfg,tokens,struct.pack('<Q',len(encoded))+encoded+body)
    return cfg,tokens

class QjmFormatTests(unittest.TestCase):
    def test_container_capabilities_size_and_sections(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'test.qjm';fixture(p);loaded=read_qjm(p,expected='tongbian');self.assertEqual(loaded['capability'],'p2c');self.assertEqual(loaded['bytes'],p.stat().st_size)
            with self.assertRaises(ValueError):read_qjm(p,expected='zhiwei')
            fixture(p,False);self.assertEqual(read_qjm(p,expected='zhiwei')['capability'],'rerank')
    def test_corruption_and_model_limits_are_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'test.qjm';fixture(p);data=bytearray(p.read_bytes());struct.pack_into('<Q',data,32+8,33);p.write_bytes(data)
            with self.assertRaises(ValueError):read_qjm(p)
            fixture(p);p.write_bytes(p.read_bytes()[:-1])
            with self.assertRaises(ValueError):read_qjm(p)
            fixture(p);data=bytearray(p.read_bytes());data[:8]=b'BADMAGIC';p.write_bytes(data)
            with self.assertRaises(ValueError):read_qjm(p)
    def test_import_preserves_original_and_different_previous_model(self):
        from neural_manager import install,model_path
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'models';p=Path(d)/'source.qjm';fixture(p);original=p.read_bytes();install(root,'tongbian',p);dest=model_path(root,'tongbian');self.assertEqual(dest.read_bytes(),original);self.assertEqual(p.read_bytes(),original)
            # A new valid metadata tag changes hash, but same supported tensor model.
            changed=original.replace(b'Artificial unit fixture',b'Artificial unit updated');p.write_bytes(changed);install(root,'tongbian',p)
            self.assertEqual(dest.read_bytes(),changed);self.assertEqual(next(dest.parent.glob('*.bak')).read_bytes(),original)
    def test_invalid_import_never_replaces_previous_file(self):
        from neural_manager import install,model_path
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'models';p=Path(d)/'source.qjm';fixture(p);install(root,'tongbian',p);original=model_path(root,'tongbian').read_bytes();p.write_bytes(b'bad file')
            with self.assertRaises(ValueError):install(root,'tongbian',p)
            self.assertEqual(model_path(root,'tongbian').read_bytes(),original);self.assertEqual(p.read_bytes(),b'bad file')
    def test_status_module_import_and_catalog_do_not_download(self):
        with patch('urllib.request.urlopen',side_effect=AssertionError('must not contact any server')):
            from neural_manager import CATALOG,entry
            self.assertEqual(entry('desktop')['bytes'],25480184)
            self.assertEqual(entry('tongbian')['downloadBytes'],143814324)
            self.assertEqual(len(CATALOG),3)

@unittest.skipUnless(importlib.util.find_spec('torch') and importlib.util.find_spec('safetensors'),'Existing torch runtime not installed; no test downloads dependencies')
class QjmInferenceTests(unittest.TestCase):
    def setUp(self):
        import torch
        self.torch=torch
        from neural_worker import CharLm,Adapter
        from safetensors.torch import save
        self.tmp=tempfile.TemporaryDirectory();p=Path(self.tmp.name)/'fixture.qjm';cfg,tokens=fixture(p)
        torch.manual_seed(123);model=CharLm(cfg)
        # Deliberate tiny owned network, with real transformer shapes / tied output embedding.
        with torch.no_grad():
            for v in model.parameters():v.zero_()
            for b in model.blocks:b.ln1.weight.fill_(1);b.ln2.weight.fill_(1)
            model.ln_f.weight.fill_(1)
            model.tok_emb.weight[6,0]=1;model.tok_emb.weight[7,1]=1;model.tok_emb.weight[2,2]=1
            model.pos_emb.weight[3,0]=10;model.pos_emb.weight[4,1]=10;model.pos_emb.weight[5,2]=10
        pack(p,cfg,tokens,save(model.state_dict()));self.adapter=Adapter(read_qjm(p,True,'tongbian'))
    def tearDown(self):self.tmp.cleanup()
    def test_real_tiny_transformer_beam_generates_nihao_contract(self):
        self.assertEqual(self.adapter.run({'task':'pinyin','segmented_pinyin':['ni'],'candidate_limit':1}),{'candidates':[{'text':'你好','pinyin':''}]})
        with self.assertRaises(ValueError):self.adapter.run({'task':'question','question':'三个木'})
        with self.assertRaises(ValueError):self.adapter.run({'task':'completion','before':'你好'})
    def test_cached_forward_equals_full_sequence(self):
        t=self.torch;a=self.adapter
        full,_=a.model(a.tensor([[2,4,5,3,6,7]]));first,cache=a.model(a.tensor([[2,4,5,3]]));step,cache=a.model(a.tensor([[6,7]]),cache)
        self.assertTrue(t.allclose(full[:,-2:],step,atol=1e-5,rtol=1e-5));self.assertEqual(cache[0][0].shape[2],6)
    def test_real_tiny_model_reranks_originals_without_inventing_text(self):
        a=self.adapter;a.capability='rerank';rows=[{'text':'你你','pinyin':'ni ni'},{'text':'你好','pinyin':'ni hao'}];ranked=a.rerank('',rows)
        self.assertEqual(set(x['text'] for x in ranked),{'你你','你好'});self.assertEqual({x['text']:x['pinyin'] for x in ranked},{x['text']:x['pinyin'] for x in rows})
        with self.assertRaises(ValueError):a.rerank('',[])
    def test_cancel_bounds_generation(self):
        with self.assertRaises(InterruptedError):self.adapter.convert('ni',1,cancelled=lambda:True)

if __name__=='__main__':unittest.main()
