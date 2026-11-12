import struct,sys
from PIL import Image,ImageDraw,ImageFont
from fontTools.ttLib import TTFont
f=open('font.bcfnt','rb').read()
tg=f.index(b'TGLP')
cw,ch,bl,mw,ssz,ns,fmt,row,col,sw,sh,doff=struct.unpack_from('<BBbBIHHHHHHI',f,tg+8)
print('baseline',bl)
def px(sheet,x,y):
    base=doff+sheet*ssz; t=(y//8)*(sw//8)+x//8; lx,ly=x%8,y%8; m=0
    for b in range(3): m|=((lx>>b)&1)<<(2*b)|((ly>>b)&1)<<(2*b+1)
    i=t*64+m; v=f[base+i//2]; return (v>>4) if i%2 else (v&0xf)
def cell(idx): 
    s=idx//5; gx=(idx%5)*(cw+1)+1
    return [[px(s,gx+x,1+y) for x in range(cw)] for y in range(ch)]
def bbox(c):
    ys=[y for y in range(len(c)) if any(c[y])]; xs=[x for x in range(len(c[0])) if any(r[x] for r in c)]
    return (xs[0],ys[0],xs[-1],ys[-1]) if ys else None
cmap=TTFont('/tmp/3ds/dl/fonts/mplus.ttf').getBestCmap()
want=[0x7E,0x107,0x192,0x3CE,0x451,0x3093,0x30F6,0x4F01,0x5217,0x5384,0x5451,0x5C55,0x5F41,0x624E,0x6759,0x7594,0x8CED,0x9091,0x96EB,0xFF5E]
print('mplus missing:', [hex(c) for c in want if c not in cmap])
def render(ch_,size,ox,oy):
    im=Image.new('L',(cw,ch),0); d=ImageDraw.Draw(im)
    d.text((ox,oy),ch_,font=ImageFont.truetype('/tmp/3ds/dl/fonts/mplus.ttf',size),fill=255)
    return [[im.getpixel((x,y))//17 for x in range(cw)] for y in range(ch)]
# 既存の字: を=723, 任=969(4EFB), わ=720?, 質=6032, あ?
refs={'を':723,'任':969,'質':6032,'雪':6678}
for k,i in refs.items(): print(k,'orig bbox',bbox(cell(i)), 'ink',sum(map(sum,cell(i))))
for size in (21,22,23,24):
    print(size,{k:bbox(render(k,size,0,0)) for k in refs}, {k:sum(map(sum,render(k,size,0,0))) for k in refs})
