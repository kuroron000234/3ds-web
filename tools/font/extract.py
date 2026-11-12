import re,struct,sys
src=open('/tmp/3ds/azahar/externals/open_source_archives/include/shared_font.app.romfs.h').read()
body=src[src.index('{')+1:src.rindex('}')]
d=bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',body))
print('romfs',len(d))
fd=struct.unpack_from('<I',d,0x24)[0]
# file meta: name
fm_off,fm_len=struct.unpack_from('<II',d,0x1c)
m=d[fm_off:fm_off+fm_len]
doff,dsz=struct.unpack_from('<QQ',m,8); nlen=struct.unpack_from('<I',m,0x1c)[0]
print('file',m[0x20:0x20+nlen].decode('utf-16le'),doff,dsz)
z=d[fd+doff:fd+doff+dsz]
def lz11(z):
    assert z[0]==0x11
    size=int.from_bytes(z[1:4],'little'); out=bytearray(); i=4
    while len(out)<size:
        fl=z[i]; i+=1
        for b in range(8):
            if len(out)>=size: break
            if fl&(0x80>>b):
                a=z[i]; ind=a>>4
                if ind==0:
                    n=((a&0xf)<<4|z[i+1]>>4)+0x11; disp=((z[i+1]&0xf)<<8|z[i+2])+1; i+=3
                elif ind==1:
                    n=((a&0xf)<<12|z[i+1]<<4|z[i+2]>>4)+0x111; disp=((z[i+2]&0xf)<<8|z[i+3])+1; i+=4
                else:
                    n=ind+1; disp=((a&0xf)<<8|z[i+1])+1; i+=2
                for _ in range(n): out.append(out[-disp])
            else:
                out.append(z[i]); i+=1
    return bytes(out)
f=lz11(z); print('bcfnt',len(f),f[:4])
open('font.bcfnt','wb').write(f)
codes=set()
for mm in re.finditer(b'CMAP',f):
    o=mm.start(); size,b,e,meth=struct.unpack_from('<IHHH',f,o+4)
    if size>len(f) or b>e: continue
    p=o+0x14
    if meth==0: codes.update(range(b,e+1))
    elif meth==1:
        for k in range(e-b+1):
            if struct.unpack_from('<H',f,p+2*k)[0]!=0xFFFF: codes.add(b+k)
    elif meth==2:
        n=struct.unpack_from('<H',f,p)[0]
        for k in range(n):
            c,ix=struct.unpack_from('<HH',f,p+2+4*k)
            if ix!=0xFFFF: codes.add(c)
print('glyph codes',len(codes))
def cnt(a,b): return sum(1 for c in codes if a<=c<=b)
print('hiragana',cnt(0x3041,0x3096),'/86  katakana',cnt(0x30A1,0x30FA),'/90  kanji(CJK)',cnt(0x4E00,0x9FFF),' fullwidth',cnt(0xFF01,0xFF5E),' halfkana',cnt(0xFF61,0xFF9F),' punct',cnt(0x3000,0x303F))
jis1="亜唖娃阿哀愛挨姶逢葵茜穐悪握渥旭葦芦鯵梓圧斡扱宛姐虻飴絵綾鮎或粟袷安庵按暗案闇鞍杏以伊位依偉囲夷委威尉惟意慰易椅為畏異移維緯胃萎衣謂違遺医井亥域育郁磯一壱溢逸稲茨芋鰯允印咽員因姻引飲淫胤蔭"
print('JIS L1 sample', sum(1 for ch in jis1 if ord(ch) in codes),'/',len(jis1))
missing=[ch for ch in "遊曲選択設定開始終了音楽練習記録戻決次前難易度完璧平凡再挑戦" if ord(ch) not in codes]
print('missing common:', ''.join(missing))
print('missing hira:', ''.join(chr(c) for c in range(0x3041,0x3097) if c not in codes), ' kata:', ''.join(chr(c) for c in range(0x30A1,0x30FB) if c not in codes))
print('PUA E000-E0FF', cnt(0xE000,0xE0FF), [hex(c) for c in sorted(codes) if 0xE000<=c<=0xF8FF][:80])
sym="♪♫★☆○◎●×△□◇→←↑↓…ー「」『』！？～・♥♡♭♯①②③④⑤％＆＋－＝℃㎝々〆〇ヶヵゝゞヽヾ、。，．：；"
print('missing symbols:', ''.join(ch for ch in sym if ord(ch) not in codes))
rng={}
for c in sorted(codes): rng.setdefault(c>>8,0); rng[c>>8]+=1
print({hex(k):v for k,v in rng.items() if k<0x4e or k>0x9f})
