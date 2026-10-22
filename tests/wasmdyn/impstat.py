import sys

exec(open(__import__('os').path.join(__import__('os').path.dirname(__file__), 'dup-syms.py')).read().split("where = defaultdict")[0])
def leb(b,p):
    v=s=0
    while True:
        c=b[p];p+=1;v|=(c&0x7f)<<s;s+=7
        if not c&0x80:return v,p
def imports(path):
    b=open(path,'rb').read();p=8;out=[]
    while p<len(b):
        sid=b[p];p+=1;n,p=leb(b,p);end=p+n
        if sid==2:
            c,q=leb(b,p)
            for _ in range(c):
                l,q=leb(b,q);m=b[q:q+l].decode();q+=l
                l,q=leb(b,q);nm=b[q:q+l].decode();q+=l
                k=b[q];q+=1
                if k==0:_,q=leb(b,q)
                elif k==1:q+=1;f=b[q];q+=1;_,q=leb(b,q); 
                elif k==2:
                    f=b[q];q+=1;_,q=leb(b,q)
                    if f&1:_,q=leb(b,q)
                elif k==3:q+=2
                elif k==4:q+=1;_,q=leb(b,q)
                out.append((m,nm,k))
            return out
        p=end
for path in sys.argv[1:]:
    own={n for n,_ in syms(path)}
    im=imports(path)
    def cnt(pred): return sum(1 for x in im if pred(x))
    fn=cnt(lambda x:x[0]=='env' and x[2]==0)
    fnself=cnt(lambda x:x[0]=='env' and x[2]==0 and x[1] in own)
    gf=cnt(lambda x:x[0]=='GOT.func'); gfs=cnt(lambda x:x[0]=='GOT.func' and x[1] in own)
    gm=cnt(lambda x:x[0]=='GOT.mem'); gms=cnt(lambda x:x[0]=='GOT.mem' and x[1] in own)
    print("%-32s imports %5d: env fn %4d (self %4d) GOT.func %4d (self %4d) GOT.mem %4d (self %4d)"%(path.split('/')[-1],len(im),fn,fnself,gf,gfs,gm,gms))
