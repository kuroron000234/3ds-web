import struct,sys
from PIL import Image,ImageDraw,ImageFont
exec(open('calib.py').read().split("cmap=TTFont")[0].replace("print('baseline',bl)",""))
from fontTools.ttLib import TTFont
mp=TTFont('/tmp/3ds/dl/fonts/mplus.ttf').getBestCmap()
S=4
LATIN=18
fm=ImageFont.truetype('/tmp/3ds/dl/fonts/mplus.ttf',21*S)
fn=ImageFont.truetype('/tmp/3ds/dl/fonts/noto.ttf',21*S); fn.set_variation_by_axes([350])
def glyph(code, full):
    c=chr(code); font=fm if code in mp else fn
    if not full:
        # 欧文は元フォントの字面が小さい（a の高さ 11px、ベースラインは 20 行目）
        big=Image.new('L',(40*S,40*S),0)
        ImageDraw.Draw(big).text((4*S,20*S),c,font=ImageFont.truetype('/tmp/3ds/dl/fonts/mplus.ttf',LATIN*S),fill=255,anchor='ls')
        sm=big.resize((40,40),Image.LANCZOS); bb=sm.point(lambda v:255 if v>=40 else 0).getbbox()
        im=Image.new('L',(cw,ch),0); im.paste(sm.crop((bb[0],0,40,40)),(0,0))
        return [[min(15,(im.getpixel((x,y))+8)//17) for x in range(cw)] for y in range(ch)]
    big=Image.new('L',(40*S,40*S),0); ImageDraw.Draw(big).text((4*S,0),c,font=font,fill=255)
    # M+ の全角の字面: x 0..21, y 6..25（21px 時）→ 元フォントの 20x23 に合わせる
    box=big.crop((4*S,6*S,4*S+32*S,6*S+19*S+8*S))
    w=round(32*20/21); h=round(27*23/19)
    sm=box.resize((w,h),Image.LANCZOS)
    im=Image.new('L',(cw,ch),0)
    ox=0
    if not full:
        bb=sm.getbbox(); ox=-bb[0] if bb else 0
    im.paste(sm,(ox,0))
    return [[min(15,(im.getpixel((x,y))+8)//17) for x in range(cw)] for y in range(ch)]
def show(c):
    for r in c: print(''.join(' .:*#'[min(4,v//3)] for v in r))
if __name__=='__main__':
    table=[(0x007E,94),(0x0107,198),(0x0192,278),(0x03CE,372),(0x0451,438),(0x3093,724),(0x30F6,814),(0x4F01,970),(0x5217,1266),(0x5384,1419),(0x5451,1508),(0x5C55,2121),(0x5F41,2364),(0x624E,2675),(0x6759,3122),(0x7594,4213),(0x8CED,6033),(0x9091,6317),(0x96EB,6679),(0xFF5E,7431)]
    out=['// Generated for 3ds-web: glyphs missing from the open source shared font.',
         '// Rendered from M PLUS 1p (Copyright 2016 The M+ Project Authors) and, for U+5F41, U+6759',
         '// and U+7594, Noto Sans JP (Copyright 2014-2021 Adobe), both under the SIL Open Font',
         '// License 1.1. Each glyph is a 24x30 cell, 4 bits of alpha per pixel, row by row, the low',
         '// nibble being the left pixel of each pair.','',
         'struct SharedFontPatchGlyph {','    u16 code;','    u16 index;','    u8 alpha[360];','};','',
         'static constexpr std::array<SharedFontPatchGlyph, %d> SHARED_FONT_PATCH_GLYPHS{{'%len(table)]
    for code,idx in table:
        g=glyph(code, code>=0x3000)
        assert any(map(any,g)), hex(code)
        b=[]
        for r in g:
            for x in range(0,24,2): b.append(r[x]|r[x+1]<<4)
        out.append('    {0x%04X, %d, {'%(code,idx))
        for k in range(0,360,20): out.append('        '+' '.join('0x%02X,'%v for v in b[k:k+20]))
        out.append('    }},')
        w=max(x for r in g for x in range(24) if r[x])+1
        print(hex(code),chr(code),'bbox',bbox(g))
    out.append('}};'); out.append('')
    open('/tmp/3ds/azahar/src/core/hle/service/apt/bcfnt/shared_font_patch.inc','w').write('\n'.join(out))
