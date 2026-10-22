import sys
def leb(b,p):
    v=s=0
    while True:
        c=b[p];p+=1;v|=(c&0x7f)<<s;s+=7
        if not c&0x80:return v,p
def needed(path):
    b=open(path,'rb').read()
    p=8
    out=[]
    while p<len(b):
        sid=b[p];p+=1
        n,p=leb(b,p)
        end=p+n
        if sid==0:
            l,q=leb(b,p);name=b[q:q+l];q+=l
            if name==b'dylink.0':
                while q<end:
                    t=b[q];q+=1
                    sl,q=leb(b,q);se=q+sl
                    if t==2:
                        c,q2=leb(b,q)
                        for _ in range(c):
                            l,q2=leb(b,q2);out.append(b[q2:q2+l].decode());q2+=l
                    q=se
            return out
        p=end
    return out
if __name__ == "__main__":
  for f in sys.argv[1:]:
      print(f.split('/')[-1], needed(f))
