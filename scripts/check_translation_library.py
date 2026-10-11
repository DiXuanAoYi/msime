"""Verify a built learning/industry library and its independently exportable sources."""
import argparse, csv, hashlib, json, sqlite3
from contextlib import closing
from pathlib import Path


def verify(root):
    catalog=json.loads((root/'translation-library/catalog.json').read_text(encoding='utf-8'))
    assert len(catalog)==36 and len({b['id'] for b in catalog})==36
    expected={'study-zk':1603,'study-gk':3677,'study-cet4':3849,'study-cet6':5407,
              'study-ky':4801,'study-ielts':5040,'study-toefl':6974,'study-gre':7504}
    with closing(sqlite3.connect((root/'industry-learning.db').resolve().as_uri()+'?mode=ro',uri=True)) as db:
        assert db.execute('PRAGMA integrity_check').fetchone()[0]=='ok'
        for book in catalog:
            path=root/'translation-library'/(book['id']+'.tsv')
            raw=path.read_bytes()
            assert len(raw)==book['bytes'] and hashlib.sha256(raw).hexdigest()==book['sha256'],book['id']
            text=raw.decode('utf-8-sig')
            lines=text.splitlines()
            assert book['source'] in '\n'.join(lines[:4])
            assert len(lines)-4==book['entries'],book['id']
            if book['id'] in expected: assert book['headwords']==expected[book['id']],book['id']
            if book['category']!='通用':
                count=db.execute('SELECT count(*) FROM glosses WHERE pack=?',(book['id'],)).fetchone()[0]
                assert count==book['entries'],book['id']
                # Compare exported records against the actual runtime lookup database.
                for line in lines[4::max(1,(len(lines)-4)//11)]:
                    language,word,gloss=line.split('\t')
                    found=db.execute('SELECT gloss FROM glosses WHERE pack=? AND language=? AND word=?',(book['id'],language,word)).fetchone()
                    assert found and found[0]==gloss,(book['id'],word)
        assert db.execute("SELECT count(*) FROM glosses WHERE word='学习' AND language='en'").fetchone()[0]>1
        assert db.execute("SELECT count(*) FROM glosses WHERE word='algorithm' AND language='zh'").fetchone()[0]>0
    print('PASS: 36 catalogs/exports; 8 pinned exam counts; bidirectional runtime rows; integrity and SHA-256')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--data-dir',type=Path,required=True)
    verify(parser.parse_args().data_dir)
