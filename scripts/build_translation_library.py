"""Build separately switchable/exportable dictionaries from pinned open data."""
import argparse
import csv
import gzip
import hashlib
import io
import json
import re
import shutil
import sqlite3
from contextlib import closing
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STUDY = {'zk': '中考 / 初中', 'gk': '高考 / 高中', 'cet4': '大学英语四级',
         'cet6': '大学英语六级', 'ky': '考研英语', 'ielts': '雅思',
         'toefl': '托福', 'gre': 'GRE'}
DOMAINS = {'机': ('mechanical', '机械制造'), '电': ('electrical', '电子电气'),
           '计': ('computing', '计算机'), '医': ('medical', '医学'), '化': ('chemistry', '化学化工'),
           '经': ('finance', '金融财经'), '法': ('law', '法律'), '建': ('construction', '建筑工程'),
           '生': ('biology', '生物'), '生化': ('biochemistry', '生物化学'), '药': ('pharmacy', '药学'),
           '物': ('physics', '物理'), '数': ('math', '数学'), '地质': ('geology', '地质'),
           '矿': ('mining', '矿业'), '天': ('astronomy', '天文'), '植': ('botany', '植物'),
           '动': ('zoology', '动物')}


def reverse_terms(translation):
    """Only exact, short Chinese senses; never invert explanatory sentences."""
    text = re.sub(r'\[[^\]]*\]|\([^)]*\)|（[^）]*）', '', translation)
    text = re.sub(r'\b(?:n|v|vt|vi|adj|adv|a|prep|pron|conj|int)\.', '', text)
    return sorted({s.strip() for s in re.split(r'[,;，；\n]', text)
                   if re.fullmatch(r'[\u4e00-\u9fff]{2,16}', s.strip())})


def build(output: Path, english_db: Path):
    source = ROOT / 'assets/translation-library/sources'
    manifest = json.loads((source / 'sources.json').read_text(encoding='utf-8'))
    raw = gzip.decompress((source / 'ecdict.csv.gz').read_bytes())
    assert hashlib.sha256(raw).hexdigest() == manifest['ecdict']['sha256'], 'ECDICT checksum mismatch'
    assert hashlib.sha256((source / 'cncf-titles.json').read_bytes()).hexdigest() == manifest['cncf']['sha256']
    packs = {}; meta = {}

    def book(id, title, category, provider='ecdict'):
        packs[id] = {}
        origin = manifest[provider]
        meta[id] = dict(id=id, name=title, category=category, language='中英双向',
                        source=origin['repository'], version=origin['commit'], license=origin['license'],
                        note='依据上游历史标签筛选，非最新官方考试大纲；词典间可能重叠。' if category == '学习' else
                        '按原始专业标签筛选；仅将简短、完整的中文释义建立反向索引。')

    for tag, name in STUDY.items(): book('study-'+tag, name, '学习')
    for n in (3000, 10000, 20000): book('frequency-'+str(n), f'英语高频前 {n}（BNC）', '学习')
    for n in (3000,10000,20000): meta['frequency-'+str(n)]['note']='按 BNC 高频排名分级，仅收录有释义的词；不同级别包含重叠词。'
    for id, name in DOMAINS.values(): book('industry-'+id, name, '行业')
    book('industry-cloud', '云原生 / 云计算（CNCF）', '行业', 'cncf')
    meta['industry-cloud']['note']='CNCF 官方术语页面中英标题配对；包含双向索引，未复制原文定义。'

    def add(id, language, word, gloss):
        key = (language, word)
        previous = packs[id].get(key, '')
        if not previous: packs[id][key] = gloss
        elif gloss not in previous.split('; '): packs[id][key] = previous + '; ' + gloss

    for row in csv.DictReader(io.StringIO(raw.decode('utf-8-sig'))):
        word = row['word'].strip()
        translation = row['translation'].replace('\\n', '\n').strip()
        if not word or not translation or len(word) > 120 or len(translation.encode()) > 4000: continue
        if any(c in word for c in '\t\r\n'): continue
        ids = ['study-'+tag for tag in row['tag'].split() if tag in STUDY]
        rank = int(row['bnc'] or 0)
        ids += ['frequency-'+str(n) for n in (3000,10000,20000) if 0 < rank <= n]
        ids += ['industry-'+DOMAINS[tag][0] for tag in set(re.findall(r'\[([^\]]+)\]',translation)) if tag in DOMAINS]
        senses = reverse_terms(translation)
        display = re.sub(r'[\t\r\n]+', '; ', translation)
        for id in ids:
            add(id, 'zh', word.lower(), display)
            for sense in senses: add(id, 'en', sense, word)
    for zh,en in json.loads((source/'cncf-titles.json').read_text(encoding='utf-8')):
        add('industry-cloud','en',zh,en)
        add('industry-cloud','zh',en.lower(),zh)

    output.mkdir(parents=True, exist_ok=True)
    destination = output / 'translation-library'
    destination.mkdir(exist_ok=True)
    dbpath = output/'industry-learning.db'
    temporary = output/'industry-learning.db.build'
    if temporary.exists(): temporary.unlink()
    with closing(sqlite3.connect(temporary)) as db, db:
        db.execute('CREATE TABLE glosses(language TEXT, word TEXT, pack TEXT, gloss TEXT NOT NULL, PRIMARY KEY(language,word,pack)) WITHOUT ROWID')
        for id, entries in packs.items():
            db.executemany('INSERT INTO glosses VALUES(?,?,?,?)', ((lang,word,id,gloss) for (lang,word),gloss in sorted(entries.items())))
        assert db.execute('PRAGMA integrity_check').fetchone()[0]=='ok'
    temporary.replace(dbpath)

    # Qingjian's original database remains unchanged; export in one documented TSV format.
    qj=ROOT/'assets/qingjian-glossary'
    qmeta=json.loads((qj/'source.json').read_text(encoding='utf-8'))
    for lang,title in {'en':'青简 · 中译英','ja':'青简 · 中译日','es':'青简 · 中译西','vi':'青简 · 中译越','zh':'青简 · 英译中'}.items():
        data=(qj/f'glossary-{lang}.tsv').read_bytes()
        assert hashlib.sha256(data).hexdigest()==qmeta['files'][f'glossary-{lang}.tsv']['sha256']
        id='qingjian-'+lang; packs[id]={}
        for line in data.decode().splitlines():
            if not line or line.startswith('#'): continue
            fields=line.split('\t'); gloss='; '.join(x.replace('|',' / ') for x in fields[1:3] if x)
            packs[id][(lang,fields[0])]=gloss
        meta[id]=dict(id=id,name=title,category='通用',language=title.split(' · ')[1],source=qmeta['repository'],version=qmeta['commit'],license=qmeta['license'],note='青简机器生成释义；原始词表和许可随包保留。')
    if english_db.exists():
        id='legacy-en'; packs[id]={}
        with closing(sqlite3.connect(english_db.as_uri()+'?mode=ro',uri=True)) as db:
            for lang,table in [('en','zh_en_glosses'),('zh','en_zh_glosses')]:
                # Schema is fixed by the original product dictionary lock.
                for word,gloss in db.execute(f'SELECT * FROM {table}'):
                    packs[id][(lang,word)]=gloss
        meta[id]=dict(id=id,name='水杉 · 基础中英释义',category='通用',language='中英双向',source='https://github.com/metasequoiaime/msime-dict',version='product-lock.json',license='见随包第三方声明',note='保留原基础翻译；关闭不影响拼音选词。')
    for id, entries in packs.items():
        m=meta[id]
        text=f"# {m['name']}\n# Source: {m['source']} @ {m['version']}\n# License: {m['license']}\n# Adaptation: filtered and converted to TSV; language\tsource\ttranslation\n"
        text += ''.join('\t'.join([lang,word,re.sub(r'[\t\r\n]+','; ',gloss)])+'\n' for (lang,word),gloss in sorted(entries.items()))
        raw=text.encode('utf-8-sig'); (destination/(id+'.tsv')).write_bytes(raw)
        m.update(entries=len(entries),headwords=sum(1 for lang,_ in entries if lang=='zh') if id.startswith(('study-','frequency-','industry-')) else len(entries),bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest())
    (destination/'catalog.json').write_text(json.dumps(list(meta.values()),ensure_ascii=False,indent=2),encoding='utf-8')
    for file in source.iterdir():
        if file.suffix != '.gz': shutil.copyfile(file,destination/file.name)
    print(json.dumps({id:len(entries) for id,entries in packs.items()},ensure_ascii=False))


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--english-db',type=Path,required=True);a=p.parse_args()
    build(a.output,a.english_db.resolve())
