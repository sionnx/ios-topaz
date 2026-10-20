import struct,sys,re
body=open(sys.argv[1],'rb').read()
def cstr(o):
    if not 0<=o<len(body): return None
    e=body.find(b'\0',o); s=body[o:e]
    return s.decode() if s and all(32<=c<127 for c in s) else None
S=body.find(b'\0flashing unlock\0')+1
hits=[]
for o in range(0,len(body)-8,8):
    q=struct.unpack_from('<Q',body,o)[0]
    if q>>48==0xffff and ((q-S)&0xfff)==0:
        hits.append((o,q-S))
for o,base in hits:
    # walk entries of 0x18 backwards/forwards while name resolves
    def ent(p):
        w2,w3,nm,h=struct.unpack_from('<IIQQ',body,p)
        n=cstr(nm-base) if nm>>48==0xffff else None
        return n,w2,w3,h
    p=o-8
    while p-0x18>=0 and ent(p-0x18)[0]: p-=0x18
    print(f"table @lk+{p:#x} base {base:#x}")
    while ent(p)[0]:
        n,w2,w3,h=ent(p); print(f"  {n!r:32s} w2={w2} w3={w3} handler=lk+{h-base:#x}"); p+=0x18
