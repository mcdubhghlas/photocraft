#version 330 core
/* Photoshop scalar blend variants, matching color/blend.c and compose/psblend.c. */
uniform sampler2D backdrop, source, maskTexture;
uniform int mode, hdr, hasMask, labMix;
#ifndef PC_KERNEL
#define PC_KERNEL 0
#endif
const int kernel=PC_KERNEL;
uniform ivec2 offset, clipMin, clipMax;
uniform sampler2D adjustLut;
uniform int adjustKind, hasOwn, oneChannel;
uniform sampler2D ownTexture;
uniform vec4 p0,p1,p2,p3,depthParams;
uniform vec4 params; /* opacity, Fill, unused, unused */
out vec4 color;
float burn(float b, float s) { return b >= 1.0-1e-4 ? 1.0 : s <= 0.0 ? 0.0 : 1.0-min((1.0-b)/s,1.0); }
float dodge(float b, float s) { return b <= 1e-4 ? 0.0 : s >= 1.0 ? 1.0 : min(b/(1.0-s),1.0); }
float vivid(float b,float s) { return s<=.5 ? (s<=0?0:1-min((1-b)/(2*s),1)) : (s>=1?1:min(b/(2*(1-s)),1)); }
float hardmix(float b,float s) { return (s<=.5?burn(b,2*s):dodge(b,2*(s-.5))) >= .5-1e-6 ? 1.0 : 0.0; }
float hardlight(float b,float s) { if(s<=.5)return b*2*s;float t=2*s-1;return b+t-b*t; }
float reflectColor(float b,float s) { return s>=1?1:min(b*b/(1-s),1); }
float channel(float b,float s) {
    float top=hdr!=0?3.402823466e38:1;
    if(mode==3)return min(b,s);
    if(mode==4)return b*s;
    if(mode==5)return burn(b,s);
    if(mode==6)return max(b+s-1,0);
    if(mode==8)return max(b,s);
    if(mode==9)return b+s-b*s;
    if(mode==10)return dodge(b,s);
    if(mode==11)return min(b+s,top);
    if(mode==13)return hardlight(s,b);
    if(mode==14)return s<=.5?2*b*s+b*b*(1-2*s):2*b*(1-s)+sqrt(max(b,0))*(2*s-1);
    if(mode==15)return hardlight(b,s);
    if(mode==16)return vivid(b,s);
    if(mode==17)return clamp(b+2*s-1,0,1);
    if(mode==18)return s<=.5?min(b,2*s):max(b,2*s-1);
    if(mode==19)return hardmix(b,s);
    if(mode==20)return abs(b-s);
    if(mode==21)return b+s-2*b*s;
    if(mode==22)return max(b-s,0);
    if(mode==23)return s<=0?(b<=0?0:1):min(b/s,top);
    if(mode==28)return reflectColor(b,s);
    if(mode==29)return reflectColor(s,b);
    if(mode==30)return 1-abs(1-b-s);
    if(mode==31)return float(uint(floor(clamp(b,0,1)*255+.5))^uint(floor(clamp(s,0,1)*255+.5)))/255;
    if(mode==32)return s<=0?0:max(1-(1-b)/s,0);
    if(mode==33)return s>=1?1:min(b/(1-s),1);
    return s;
}
float lum(vec3 c){return .3*c.r+.59*c.g+.11*c.b;}
float sat(vec3 c){return max(max(c.r,c.g),c.b)-min(min(c.r,c.g),c.b);}
vec3 setLumLimit(vec3 c,float l,float top){
    c+=l-lum(c);
    float n=min(min(c.r,c.g),c.b),x=max(max(c.r,c.g),c.b);
    if(n<0){float d=l-n;c=l>0&&d>0?l+(c-l)*l/d:vec3(l);}
    if(x>top){float d=x-l;c=l<top&&d>0?l+(c-l)*(top-l)/d:vec3(l);}
    return c;
}
vec3 setLum(vec3 c,float l){return setLumLimit(c,l,hdr!=0?3.402823466e38:1);}
vec3 setSat(vec3 c,float s){
    int index[3]=int[3](0,1,2);
    for(int i=1;i<3;i++){int t=index[i],j=i;while(j>0&&c[t]<c[index[j-1]]){index[j]=index[j-1];j--;}index[j]=t;}
    vec3 outColor=vec3(0);
    if(c[index[2]]>c[index[0]]){outColor[index[1]]=(c[index[1]]-c[index[0]])*s/(c[index[2]]-c[index[0]]);outColor[index[2]]=s;}
    return outColor;
}
vec3 blend(vec3 b,vec3 s){
    if(mode==7)return lum(s)<lum(b)?s:b;
    if(mode==12)return lum(s)>lum(b)?s:b;
    if(mode==24)return setLum(setSat(s,sat(b)),lum(b));
    if(mode==25)return setLum(setSat(b,sat(s)),lum(b));
    if(mode==26)return setLum(s,lum(b));
    if(mode==27)return setLum(b,lum(s));
    return vec3(channel(b.r,s.r),channel(b.g,s.g),channel(b.b,s.b));
}
bool special(){return mode==5||mode==6||mode==10||mode==11||mode==16||mode==17||mode==19||mode==20;}
vec3 blendFill(vec3 b,vec3 s,float f){
    if(mode==19)return clamp((b-f*(1-s))/(1-f),0,1);
    float neutral=mode==5||mode==6?1:mode==16||mode==17?.5:0;
    return blend(b,vec3(neutral)+(s-neutral)*f);
}

float decode(float v,float g){v=max(v,0);if(g>0)return pow(max(v,0),g);return v<=.04045?v/12.92:pow((v+.055)/1.055,2.4);}
float encode(float v,float g){v=max(v,0);if(g>0)return pow(max(v,0),1/g);return v<=.0031308?v*12.92:1.055*pow(v,1/2.4)-.055;}
float exposure(float v){
    if(hdr!=0){float l=v*p0.x+p0.y;return p0.z==1?l:sign(l)*pow(abs(l),1/p0.z);}
    return clamp(encode(pow(max(decode(v,p0.w)*p0.x+p0.y,0),1/p0.z),p0.w),0,1);
}
float mbright(float v,float b){
    if(b==0)return v;float s=pow(1.375,b/50),p=b>=0?max(4.5-.013*b,2):5-.072*b;
    v=clamp(v,0,1);return clamp(s*v+(1-s)*pow(v,p),0,1);
}
float mcontrast(float v,float ct){
    if(ct==0)return v;float k=ct/128,e=1-k,m=1+k;v=clamp(v,0,1);
    float t=v<=.5?v/.5:(1-v)/.5;
    float h=e*.5*(t*t*t-2*t*t+t)+(-2*t*t*t+3*t*t)*.5+m*.5*(t*t*t-t*t);
    return v<=.5?h:1-h;
}
float tone(int ch,float v){
    if(p0[ch]>.5)return v;
    float x=clamp(v,0,1)*4095;int low=int(floor(x));
    float a=texelFetch(adjustLut,ivec2(low,0),0)[ch],b=texelFetch(adjustLut,ivec2(min(low+1,4095),0),0)[ch];
    return a+(b-a)*(x-float(low));
}
float levels(int rec,float v){
    float black=texelFetch(adjustLut,ivec2(0,0),0)[rec],white=texelFetch(adjustLut,ivec2(1,0),0)[rec];
    float gamma=texelFetch(adjustLut,ivec2(2,0),0)[rec],low=texelFetch(adjustLut,ivec2(3,0),0)[rec],high=texelFetch(adjustLut,ivec2(4,0),0)[rec];
    float t=(v-black)/max(white-black,.000001);return low+sign(t)*pow(abs(t),1/gamma)*(high-low);
}
float posterize(float v){float n=p0.x,x=floor(clamp(v,0,1)*255+.5);return floor(min(floor(x*n/256),n-1)*255/(n-1))/255;}
float rem(float x,float y){float r=mod(x,y);return r<0?r+abs(y):r;}
vec3 rgbHsl(vec3 c){
    float mx=max(max(c.r,c.g),c.b),mn=min(min(c.r,c.g),c.b),l=(mx+mn)/2,d=mx-mn;
    if(abs(d)<1e-7)return vec3(0,0,l);
    float s=l>.5?d/(2-mx-mn):d/(mx+mn),h;
    if(mx==c.r)h=rem((c.g-c.b)/d,6);else if(mx==c.g)h=(c.b-c.r)/d+2;else h=(c.r-c.g)/d+4;
    return vec3(h/6,s,l);
}
float hueCh(float p,float q,float t){t=rem(t,1);if(t<1.0/6)return p+(q-p)*6*t;if(t<.5)return q;if(t<2.0/3)return p+(q-p)*(2.0/3-t)*6;return p;}
vec3 hslRgb(float h,float s,float l){if(s<=0)return vec3(l);float q=l<.5?l*(1+s):l+s-l*s,p=2*l-q;return vec3(hueCh(p,q,h+1.0/3),hueCh(p,q,h),hueCh(p,q,h-1.0/3));}
vec3 rawLut(float v){float x=clamp(v,0,1)*4095;int low=int(floor(x));vec3 a=texelFetch(adjustLut,ivec2(low,0),0).rgb,b=texelFetch(adjustLut,ivec2(min(low+1,4095),0),0).rgb;return a+(b-a)*(x-float(low));}
vec3 hueSaturation(vec3 c){
    vec3 hsl=rgbHsl(c),ranges=rawLut(hsl.x);
    float s=clamp(p0.y+(p0.w>.5?0:ranges.y),-1,1),l=clamp(p0.z,-1,1);
    if(p0.w>.5){float ll=l>0?hsl.z+(1-hsl.z)*l:hsl.z*(1+l);return hslRgb(rem(p0.x,360)/360,min(abs(s),1),ll);}
    vec3 lc=l>0?c+(1-c)*l:c*(1+l),lh=rgbHsl(lc);
    float hh=rem(lh.x+(p0.x+ranges.x)/360,1),ss=s>0?clamp(lh.y/max(1-s,1.0/512),0,1):clamp(lh.y*(1+s),0,1);
    vec3 rgb=hslRgb(hh,ss,lh.z);float rl=clamp(ranges.z,-1,1);
    if(rl>0)rgb+=(vec3(max(max(rgb.r,rgb.g),rgb.b))-rgb)*rl;
    else if(rl<0)rgb+=(vec3(min(min(rgb.r,rgb.g),rgb.b))-rgb)*(-rl);
    return rgb;
}
vec3 vibranceLinear(vec3 c,float v){
    float mx=max(max(c.r,c.g),c.b),mn=min(min(c.r,c.g),c.b);
    if(mx<=0||mx-mn<=1e-9)return c;
    float t=1-mn/mx,shape=t*(1-t)*(2-t+t*t)*mx*max(1-mx,0),t2,v2;
    if(v<0){float a=-v;t2=t*(1-a/4)*(1-a+a*t*(1+t)/2);v2=mx-a*shape;}
    else {
        float d=mx-mn,h;
        if(mx==c.r)h=rem((c.g-c.b)/d,6);else if(mx==c.g)h=(c.b-c.r)/d+2;else h=(c.r-c.g)/d+4;
        h*=60;float w=0;if(h<=30)w=1;else if(h<45)w=(45-h)/15;else if(h>=300)w=(h-300)/60;
        float a=v*(1-w)+.64*pow(v,1.6)*w;
        float boost=(p1.x*a+p1.y*a*a)*pow(t,p1.z)*pow(1-t,p1.w)*exp((p2.x+p2.y*a+p2.z*(1-t))*max(1-mx,0));
        t2=min(t*(1+boost),1);v2=mx+v/4*shape;
    }
    float m2=v2*(1-t2),k=(v2-m2)/(mx-mn);return vec3(m2)+(c-vec3(mn))*k;
}
vec3 vibrance(vec3 c){
    vec3 l=vec3(decode(c.r,p0.z),decode(c.g,p0.z),decode(c.b,p0.z));
    if(p0.x!=0)l=vibranceLinear(l,p0.x);
    if(p0.y!=0){float g=.288*l.r+.712*l.g;l=clamp(vec3(g)+(1+p0.y)*(l-vec3(g)),0,p0.w);}
    l=clamp(l,0,p0.w);return vec3(encode(l.r,p0.z),encode(l.g,p0.z),encode(l.b,p0.z));
}
vec3 selective(vec3 c){
    float mx=max(max(c.r,c.g),c.b),mn=min(min(c.r,c.g),c.b),mid=c.r+c.g+c.b-mx-mn;
    float weights[9]=float[9](c.r>=mx?mx-mid:0,c.b<=mn?mid-mn:0,c.g>=mx?mx-mid:0,
        c.r<=mn?mid-mn:0,c.b>=mx?mx-mid:0,c.g<=mn?mid-mn:0,max((mn-.5)*2,0),
        clamp(1-abs(mx-.5)-abs(mn-.5),0,1),max((.5-mx)*2,0));
    vec3 delta=vec3(0);
    for(int r=0;r<9;r++){
        if(weights[r]<=0)continue;
        vec4 a=texelFetch(adjustLut,ivec2(r,0),0)/100;vec3 d=a.rgb+a.a;
        if(p0.x>.5)d*=1-c;
        else for(int ch=0;ch<3;ch++)d[ch]*=d[ch]>0?min(2*c[ch],1):min(2*(1-c[ch]),1);
        delta+=d*weights[r];
    }
    return clamp(c-delta,0,1);
}
float dither(){ivec2 pos=ivec2(gl_FragCoord.xy)+offset;float m[16]=float[16](0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5);return ((m[(pos.y&3)*4+(pos.x&3)]+.5)/16-.5)/255;}
vec3 lut3(int n,int r,int g,int b){int i=(b*n+g)*n+r,w=textureSize(adjustLut,0).x;return texelFetch(adjustLut,ivec2(i%w,i/w),0).rgb;}
vec3 colorLookup(vec3 c){
    int n=int(p0.x);vec3 pos=clamp(c,0,1)*float(n-1);ivec3 low=min(ivec3(floor(pos)),ivec3(n-2));
    vec3 f=pos-vec3(low);int r=low.x,g=low.y,b=low.z;
    if(p0.y>.5){
        vec3 a=lut3(n,r,g,b),z=lut3(n,r+1,g+1,b+1);
        if(f.x>f.y){
            if(f.y>f.z)return (1-f.x)*a+(f.x-f.y)*lut3(n,r+1,g,b)+(f.y-f.z)*lut3(n,r+1,g+1,b)+f.z*z;
            if(f.x>f.z)return (1-f.x)*a+(f.x-f.z)*lut3(n,r+1,g,b)+(f.z-f.y)*lut3(n,r+1,g,b+1)+f.y*z;
            return (1-f.z)*a+(f.z-f.x)*lut3(n,r,g,b+1)+(f.x-f.y)*lut3(n,r+1,g,b+1)+f.y*z;
        }
        if(f.z>f.y)return (1-f.z)*a+(f.z-f.y)*lut3(n,r,g,b+1)+(f.y-f.x)*lut3(n,r,g+1,b+1)+f.x*z;
        if(f.z>f.x)return (1-f.y)*a+(f.y-f.z)*lut3(n,r,g+1,b)+(f.z-f.x)*lut3(n,r,g+1,b+1)+f.x*z;
        return (1-f.y)*a+(f.y-f.x)*lut3(n,r,g+1,b)+(f.x-f.z)*lut3(n,r+1,g+1,b)+f.z*z;
    }
    vec3 c00=mix(lut3(n,r,g,b),lut3(n,r+1,g,b),f.x),c10=mix(lut3(n,r,g+1,b),lut3(n,r+1,g+1,b),f.x);
    vec3 c01=mix(lut3(n,r,g,b+1),lut3(n,r+1,g,b+1),f.x),c11=mix(lut3(n,r,g+1,b+1),lut3(n,r+1,g+1,b+1),f.x);
    return mix(mix(c00,c10,f.y),mix(c01,c11,f.y),f.z);
}
vec3 adjust(vec3 c){
    if(adjustKind==1)return 1-c;
    if(adjustKind==2)return vec3(floor(dot(c,vec3(.299,.587,.114))*255+.5)>=p0.x?1.0:0.0);
    if(adjustKind==3)return vec3(posterize(c.r),posterize(c.g),posterize(c.b));
    if(adjustKind==4)return clamp((c+p0.x-p0.z)*p0.y+p0.z+p0.w,0,1);
    if(adjustKind==5)return vec3(mcontrast(mbright(c.r,p0.x),p0.y),mcontrast(mbright(c.g,p0.x),p0.y),mcontrast(mbright(c.b,p0.x),p0.y));
    if(adjustKind==6)return vec3(exposure(c.r),exposure(c.g),exposure(c.b));
    if(adjustKind==8)return hueSaturation(c);
    if(adjustKind==9)return vibrance(c);
    if(adjustKind==15)return selective(c);
    if(adjustKind==16){vec3 result=colorLookup(c);if(p0.z>.5)result=clamp(result+dither(),0,1);return result;}
    if(adjustKind==13){float t=dot(c,vec3(.299,.587,.114));if(p0.x>.5)t=1-t;vec3 result=rawLut(t);if(p0.y>.5)result=clamp(result+dither(),0,1);return result;}
    if(adjustKind==7)return vec3(tone(0,c.r),tone(1,c.g),tone(2,c.b));
    if(adjustKind==17)return vec3(levels(0,levels(1,c.r)),levels(0,levels(2,c.g)),levels(0,levels(3,c.b)));
    if(adjustKind==10){float r=clamp(dot(p0.xyz,c)+p0.w,0,1);return p3.x>.5?vec3(r):vec3(r,clamp(dot(p1.xyz,c)+p1.w,0,1),clamp(dot(p2.xyz,c)+p2.w,0,1));}
    if(adjustKind==11){
        float g=p3.y;vec3 lin=vec3(decode(c.r,g),decode(c.g,g),decode(c.b,g));
        vec3 f=vec3(encode(dot(p0.xyz,lin),g),encode(dot(p1.xyz,lin),g),encode(dot(p2.xyz,lin),g));
        if(p3.x>.5)f=setLumLimit(f,lum(c),1);return clamp(f,0,1);
    }
    if(adjustKind==12){
        float w[6]=float[6](p0.x,p0.y,p0.z,p0.w,p1.x,p1.y);
        float mx=max(max(c.r,c.g),c.b),mn=min(min(c.r,c.g),c.b),mid=c.r+c.g+c.b-mx-mn;
        int primary=c.r>=c.g&&c.r>=c.b?0:c.g>=c.b?2:4;
        int secondary=c.b<=c.r&&c.b<=c.g?1:c.r<=c.g?3:5;
        float g=clamp(mn+(mid-mn)*w[secondary]/100+(mx-mid)*w[primary]/100,0,1);
        return p1.z>.5?clamp(setLumLimit(p2.rgb,g,1),0,1):vec3(g);
    }
    return c;
}



float labF(float t){return t>.008856452?pow(t,1.0/3):t/.12841855+.13793103;}
float labInv(float t){return t>.20689656?t*t*t:.12841855*(t-.13793103);}
float rawLinear(float v){return v<=.04045?v/12.92:pow((v+.055)/1.055,2.4);}
vec3 rgbLab(vec3 c){
    vec3 lin=vec3(rawLinear(c.r),rawLinear(c.g),rawLinear(c.b));
    float fx=labF((.436074*lin.r+.385064*lin.g+.143080*lin.b)/.96422);
    float fy=labF(.222504*lin.r+.716878*lin.g+.060618*lin.b);
    float fz=labF((.013932*lin.r+.097104*lin.g+.714173*lin.b)/.82521);
    return vec3(116*fy-16,500*(fx-fy),200*(fy-fz));
}
vec3 labRgb(vec3 lab){
    float fy=(lab.x+16)/116;vec3 xyz=vec3(labInv(fy+lab.y/500)*.96422,labInv(fy),labInv(fy-lab.z/200)*.82521);
    vec3 lin=clamp(vec3(3.133856*xyz.x-1.616867*xyz.y-.490615*xyz.z,
        -.978768*xyz.x+1.916142*xyz.y+.033454*xyz.z,.071945*xyz.x-.228991*xyz.y+1.405243*xyz.z),0,1);
    return vec3(encode(lin.r,0),encode(lin.g,0),encode(lin.b,0));
}
vec3 textEncode(vec3 c,float gamma){
    vec3 lin=vec3(decode(c.r,0),decode(c.g,0),decode(c.b,0));return pow(lin,vec3(1/gamma));
}
vec3 textDecode(vec3 c,float gamma){
    vec3 lin=pow(max(c,vec3(0)),vec3(gamma));return vec3(encode(lin.r,0),encode(lin.g,0),encode(lin.b,0));
}
vec3 coverageMix(vec3 b,vec3 mixed,vec3 s,float wb,float wm,float ws){
    float gamma=params.z;
    if(gamma==1)return wb*b+wm*mixed+ws*s;
    return textDecode(wb*textEncode(b,gamma)+wm*textEncode(mixed,gamma)+ws*textEncode(s,gamma),gamma);
}
float biRamp(float low,float high,float v){return high<=low?(v>=low?1.0:0.0):clamp((v-low)/(high-low),0,1);}
float biWeight(int entry,int side,float v){
    vec4 ranges[4]=vec4[4](p0,p1,p2,p3);float black=ranges[entry][side*2],white=ranges[entry][side*2+1];
    float bl=floor(black/256),bh=black-bl*256,wl=floor(white/256),wh=white-wl*256;
    return biRamp(bl,bh,v)*biRamp(255-wh,255-wl,255-v);
}
float blendIf(vec4 own,vec4 under){
    float k=1;
    for(int side=0;side<2;side++){
        vec4 pixel=side==0?own:under;if(pixel.a<=0)continue;
        vec3 v=clamp(pixel.rgb,0,1)*255;
        for(int ch=0;ch<3;ch++){float nearest=floor(v[ch]+.5);if(abs(v[ch]-nearest)<1e-4)v[ch]=nearest;}
        if(oneChannel!=0)k*=biWeight(0,side,v.r)*biWeight(1,side,v.r);
        else k*=biWeight(0,side,dot(v,vec3(.299,.587,.114)))*biWeight(1,side,v.r)*biWeight(2,side,v.g)*biWeight(3,side,v.b);
        if(k<=0)return 0.0;
    }
    return k;
}
uniform sampler2D effectMap, paintTexture, accumulateTexture;
uniform ivec2 mapOrigin, mapSize;
uniform int fxKind, fxFlags, paintKind;
uniform vec4 fxParams; // outside, knockout, glow gain, missing pattern
float effectMapValue(ivec2 point){
    ivec2 p=point-mapOrigin;
    return all(greaterThanEqual(p,ivec2(0)))&&all(lessThan(p,mapSize))?texelFetch(effectMap,p,0).r:fxParams.x;
}
vec4 effectMix(vec4 a,vec4 b,float k){
    float alpha=a.a+(b.a-a.a)*k;
    return alpha>0?vec4((a.rgb*a.a+(b.rgb*b.a-a.rgb*a.a)*k)/alpha,alpha):vec4(0);
}
vec4 effectComposite(vec4 b,vec4 s,ivec2 at){
    float a=s.a;
    if(mode==2){
        uvec2 p=uvec2(at)+uvec2(offset);
        uint n=(p.x*0x8da6b343u)^(p.y*0xd8163841u)^0x9e3779b9u;
        n^=n>>15;n*=0x2c1b3c6du;n^=n>>12;
        a=float(n&0xffffu)/65536.0<a?1.0:0.0;
    }
    if(a<=0)return b;
    float alpha=a+b.a*(1-a);
    if(labMix!=0&&(mode==1||mode==0)&&params.z==1&&b.a>0&&alpha>0)
        return vec4(labRgb(rgbLab(b.rgb)*(b.a*(1-a)/alpha)+rgbLab(s.rgb)*(a/alpha)),alpha);
    return alpha>0?vec4(coverageMix(b.rgb,blend(b.rgb,s.rgb),s.rgb,(1-a)*b.a/alpha,a*b.a/alpha,(1-b.a)*a/alpha),alpha):vec4(0);
}
void main(){
    ivec2 at=ivec2(gl_FragCoord.xy);
    vec4 b=texelFetch(backdrop,at,0),s=texelFetch(source,at,0);
    float mask=hasMask!=0?texelFetch(maskTexture,at,0).r:1.0;
    if(kernel==10){
        if(fxKind==1){color=vec4(b.rgb,1);return;}
        if(fxKind==2){color=vec4(b.rgb,b.a>.5/255.0?params.x:0);return;}
        if(fxKind==3){color=vec4(b.rgb,b.a*min(s.a,1));return;}
        if(fxKind==4){
            vec4 stroke=texelFetch(ownTexture,at,0);
            color=stroke.a>0&&s.a>.5/255.0?effectComposite(b,vec4(stroke.rgb,params.x*min(stroke.a/s.a,1)),at):b;return;
        }
        if(fxKind==5){color=vec4(b.rgb,s.a>.5/255.0?params.x*min(b.a/s.a,1):0);return;}
        if(fxKind==6){color=vec4(0,b.a,0,0);return;}
        if(fxKind==7){color=vec4(b.rgb,max(b.a,effectMapValue(at+offset)));return;}
        color=vec4(b.rgb,b.a*params.x);return;
    }
    if(kernel==11){
        float a=s.a,m=fxKind==0?effectMapValue(at+offset):1;
        if((fxFlags&1)!=0)m*=1-a*fxParams.y;
        if((fxFlags&2)!=0&&a<=.5/255.0)m=0;
        if((fxFlags&4)!=0)m=a>.5/255.0?min(m/a,1):0;
        vec4 c=texelFetch(paintTexture,at,0);
        if(paintKind==3){
            float t=1-clamp(m,0,1),u=t*float(textureSize(paintTexture,0).x-1);
            int i=int(u);c=mix(texelFetch(paintTexture,ivec2(i,0),0),texelFetch(paintTexture,ivec2(min(i+1,textureSize(paintTexture,0).x-1),0),0),fract(u));
            c.a*=min(m*fxParams.z,1)*params.x;
        }else c.a*=m*params.x;
        color=effectComposite(b,c,at);return;
    }
    if(kernel==12){
        vec4 before=texelFetch(ownTexture,at,0),w=b;
        if(s.a>0&&(fxFlags&128)==0)w=effectComposite(b,s,at);
        bool atop=(fxFlags&64)!=0;
        vec4 o=effectMix(atop?vec4(before.rgb,1):before,w,params.x);
        color=atop?(before.a>0?vec4(o.rgb,before.a):before):o;return;
    }
    if(kernel==13){
        float a=s.a;bool outline=(fxFlags&16)!=0,first=(fxFlags&32)!=0,vector=(fxFlags&8)!=0;
        vec4 cv=texelFetch(ownTexture,at,0);
        if(first&&!outline&&vector)cv=vec4(0);
        float k=effectMapValue(at+offset);
        if(outline){float rest=1-clamp(cv.g,0,1);k*=rest<=1e-6?1:min(max(1-clamp(a,0,1),0)/rest,1);}
        else if(a>.5/255.0)k=vector?0:cv.g>=1?1:clamp((1-clamp(a,0,1))/(1-cv.g),0,1);
        float share=k*params.x*(1-cv.r);
        if(fxKind==1){color=vec4(cv.r+share,cv.g,0,0);return;}
        vec4 acc=first?vec4(0):texelFetch(accumulateTexture,at,0);
        if(share>0){vec4 c=texelFetch(paintTexture,at,0),bl=fxParams.w>0?b:effectComposite(b,c,at);acc+=share*vec4(bl.rgb*bl.a,bl.a);}
        color=acc;return;
    }
    if(kernel==14){
        float cover=texelFetch(ownTexture,at,0).r;
        if(cover<=0){color=b;return;}
        float k=1-min(cover,1),alpha=b.a*k+s.a;
        color=alpha>0?vec4((b.rgb*b.a*k+s.rgb)/alpha,min(alpha,1)):b;return;
    }
    if(kernel==9){
        vec4 c=texelFetch(ownTexture,at,0),pm=vec4(b.rgb*b.a,b.a)+vec4(s.rgb*s.a,s.a)-vec4(c.rgb*c.a,c.a);
        float alpha=clamp(pm.a,0,1);color=alpha>0?vec4(clamp(pm.rgb/alpha,0,1),alpha):vec4(0);return;
    }
    if(kernel==8){ivec2 p=at+offset;color=all(greaterThanEqual(p,clipMin))&&all(lessThan(p,clipMax))?s:b;return;}
    if(kernel==6){color=b+(s-b)*p0;return;}
    if(kernel==7){
        vec4 own=hasOwn!=0?texelFetch(ownTexture,at,0):s;float k=blendIf(own,b);
        if(k>=1){color=s;return;}
        float alpha=b.a+(s.a-b.a)*k;
        color=alpha>0?vec4(clamp((b.rgb*b.a+(s.rgb*s.a-b.rgb*b.a)*k)/alpha,0,1),alpha):vec4(0);return;
    }
    if(kernel==4){color=b.a>0?vec4(adjust(b.rgb),b.a):b;return;}
    if(kernel==5){
        float k=params.x*mask;
        color=vec4(b.rgb+(blend(b.rgb,s.rgb)-b.rgb)*k,b.a);
        if(depthParams.x>0&&params.y>.5)color=floor(color*depthParams.x+.5)/depthParams.x;
        return;
    }
    if(kernel==1){color=vec4(b.rgb,b.a*mask);return;}
    if(kernel==2){
        float k=clamp(params.x*mask,0,1),alpha=mix(b.a,s.a,k);
        color=alpha>0?vec4(mix(b.rgb*b.a,s.rgb*s.a,k)/alpha,alpha):vec4(0);return;
    }
    float f=clamp(params.y,0,1),a=s.a*params.x;
    if(a<=0){color=b;return;}
    if(kernel==3){
        if(labMix!=0&&(mode==1||mode==0)&&params.z==1&&b.a>0){color=vec4(labRgb(rgbLab(b.rgb)*(1-a)+rgbLab(s.rgb)*a),b.a);return;}
        color=b.a>0?vec4(coverageMix(b.rgb,blend(b.rgb,s.rgb),s.rgb,1-a,a,0),b.a):b;return;
    }
    if(f<1&&special()){
        float alone=a*f,alpha=b.a+(1-b.a)*alone;
        vec3 result=alpha>0?coverageMix(b.rgb,blendFill(b.rgb,s.rgb,f),s.rgb,b.a*(1-a)/alpha,b.a*a/alpha,(1-b.a)*alone/alpha):vec3(0);
        color=vec4(result,alpha);return;
    }
    a*=f;
    if(mode==2){
        uvec2 p=uvec2(at)+uvec2(offset);
        uint noise=(p.x*0x8da6b343u)^(p.y*0xd8163841u)^0x9e3779b9u;
        noise^=noise>>15;noise*=0x2c1b3c6du;noise^=noise>>12;
        a=float(noise&0xffffu)/65536.0<a?1.0:0.0;
    }
    if(a<=0){color=b;return;}
    float alpha=a+b.a*(1-a);
    if(labMix!=0&&(mode==1||mode==0)&&params.z==1&&b.a>0&&alpha>0){
        color=vec4(labRgb(rgbLab(b.rgb)*(b.a*(1-a)/alpha)+rgbLab(s.rgb)*(a/alpha)),alpha);return;
    }
    vec3 result=alpha>0?coverageMix(b.rgb,blend(b.rgb,s.rgb),s.rgb,(1-a)*b.a/alpha,a*b.a/alpha,(1-b.a)*a/alpha):vec3(0);
    color=vec4(result,alpha);
}
