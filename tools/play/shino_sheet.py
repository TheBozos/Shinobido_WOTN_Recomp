"""shino_sheet.py <dir> <out.png> [columns]: a contact sheet of the PNG captures in <dir>, each labelled with its game frame."""
import sys,glob,os
from PIL import Image, ImageDraw
d,out=sys.argv[1],sys.argv[2]; cols=int(sys.argv[3]) if len(sys.argv)>3 else 6
fs=sorted(glob.glob(os.path.join(d,'*.png')))
w,h=320,240
rows=(len(fs)+cols-1)//cols
S=Image.new('RGB',(cols*w,rows*(h+14)),'white'); D=ImageDraw.Draw(S)
for i,f in enumerate(fs):
    im=Image.open(f).convert('RGB').resize((w,h))
    x,y=(i%cols)*w,(i//cols)*(h+14)
    S.paste(im,(x,y+14)); D.text((x+2,y+1),os.path.basename(f)[6:-4],fill='black')
S.save(out)
