import sys,struct,ast
def parse(path):
    ents=[];cur={};key=None;fuzzy=False
    def flush():
        nonlocal cur,fuzzy
        if 'msgid' in cur and not fuzzy:
            ents.append(cur)
        cur={};fuzzy=False
    for line in open(path,encoding='utf-8'):
        line=line.strip()
        if not line:
            flush();key=None;continue
        if line.startswith('#,') and 'fuzzy' in line: fuzzy=True;continue
        if line.startswith('#'): continue
        if line.startswith('"'):
            cur[key]+=ast.literal_eval(line);continue
        k,_,v=line.partition(' ')
        if k=='msgid' and 'msgid' in cur and 'msgstr' in cur or (k=='msgctxt' and 'msgid' in cur): flush()
        key=k;cur[k]=ast.literal_eval(v)
    flush();return ents
ents=parse(sys.argv[1]);out={}
for e in ents:
    mid=e['msgid'];
    if 'msgctxt' in e: mid=e['msgctxt']+'\x04'+mid
    if 'msgid_plural' in e:
        mid=mid+'\x00'+e['msgid_plural']
        strs=[e[k] for k in sorted([k for k in e if k.startswith('msgstr[')],key=lambda k:int(k[7:-1]))]
        if not any(strs): continue
        val='\x00'.join(strs)
    else:
        val=e.get('msgstr','')
        if not val: continue
    out[mid]=val
keys=sorted(out,key=lambda s:s.encode())
ids=b'';strs=b'';offs=[]
for k in keys:
    kb=k.encode();vb=out[k].encode()
    offs.append((len(ids),len(kb),len(strs),len(vb)));ids+=kb+b'\0';strs+=vb+b'\0'
n=len(keys);ko=28;vo=ko+n*8;idstart=vo+n*8;strstart=idstart+len(ids)
data=struct.pack('<7I',0x950412de,0,n,ko,vo,0,0)
for o,l,_,_ in offs: data+=struct.pack('<2I',l,idstart+o)
for _,_,o,l in offs: data+=struct.pack('<2I',l,strstart+o)
data+=ids+strs
open(sys.argv[2],'wb').write(data);print(n,'entradas',len(data),'bytes')
