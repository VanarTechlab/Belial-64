import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
W,H=640,300
rng=np.random.default_rng(64)
def noise(scale,seed):
    r=np.random.default_rng(seed)
    g=r.random((H//scale+2,W//scale+2))
    im=Image.fromarray((g*255).astype(np.uint8)).resize((W,H),Image.BICUBIC)
    return np.asarray(im,dtype=np.float32)/255
n=0.5*noise(40,1)+0.3*noise(16,2)+0.2*noise(6,3)
y=np.linspace(0,1,H)[:,None]
heat=np.clip((y**1.6)*1.25*(0.55+0.9*n)-0.15,0,1)
# vignette
x=np.linspace(-1,1,W)[None,:]
vig=np.clip(1-0.55*(x**2)-0.25*((1-y)**2),0,1)
heat*=vig
r=np.clip(heat*2.2,0,1); g=np.clip(heat*1.3-0.35,0,1); b=np.clip(heat*0.6-0.4,0,1)
base=np.stack([r*235+10*n,g*170+4,b*90+6],-1)
img=Image.fromarray(np.clip(base,0,255).astype(np.uint8))
# embers
d=ImageDraw.Draw(img)
for i in range(140):
    ex,ey=rng.integers(0,W),int(H*(0.35+0.65*rng.random()**0.5))
    s=rng.integers(1,3); c=(255,int(150+rng.integers(0,90)),40)
    d.ellipse([ex,ey,ex+s,ey+s],fill=c)
font=ImageFont.truetype('/usr/share/texmf/fonts/opentype/public/tex-gyre/texgyrebonum-bold.otf',92)
sub=ImageFont.truetype('/usr/share/texmf/fonts/opentype/public/tex-gyre/texgyrebonum-bold.otf',24)
small=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf',15)
t="BELIAL 64"
bb=d.textbbox((0,0),t,font=font); tw=bb[2]-bb[0]
tx=(W-tw)//2; ty=60
glow=Image.new('L',(W,H),0); ImageDraw.Draw(glow).text((tx,ty),t,font=font,fill=255)
gl=glow.filter(ImageFilter.GaussianBlur(10))
img=Image.composite(Image.new('RGB',(W,H),(255,60,0)),img,gl.point(lambda v:int(v*0.85)))
# metal gradient text
grad=np.zeros((H,W,3),np.uint8)
yy=np.linspace(0,1,H)[:,None]
top=np.array([255,236,190]);bot=np.array([150,20,8])
t01=np.clip((yy*H-ty-10)/95,0,1)
grad[:]=(top*(1-t01)[...,None]+bot*t01[...,None]).astype(np.uint8)[:, :1, :].repeat(W,1) if False else (top*(1-t01)[...,None]+bot*t01[...,None]).astype(np.uint8)
mask=Image.new('L',(W,H),0); ImageDraw.Draw(mask).text((tx,ty),t,font=font,fill=255)
shadow=mask.filter(ImageFilter.GaussianBlur(2))
img=Image.composite(Image.new('RGB',(W,H),(0,0,0)),img,shadow.point(lambda v:int(v*0.9)).transform((W,H),Image.AFFINE,(1,0,-3,0,1,-3)))
img=Image.composite(Image.fromarray(grad),img,mask)
d=ImageDraw.Draw(img)
s1="Diablo para Nintendo 64"
bb=d.textbbox((0,0),s1,font=sub); d.text(((W-(bb[2]-bb[0]))//2+1,175),s1,font=sub,fill=(0,0,0)); d.text(((W-(bb[2]-bb[0]))//2,174),s1,font=sub,fill=(240,215,160))
s2="Por Vanar  -  basado en DevilutionX"
bb=d.textbbox((0,0),s2,font=small); d.text(((W-(bb[2]-bb[0]))//2,210),s2,font=small,fill=(225,190,140))
# fade bottom to window bg (4,7,7)
arr=np.asarray(img).astype(np.float32)
f=np.clip((yy-0.82)/0.18,0,1)[...,None]
arr=arr*(1-f)+np.array([4,7,7])*f
img=Image.fromarray(arr.astype(np.uint8))
img.save('fondo.bmp'); img.save('fondo_vista.png')
