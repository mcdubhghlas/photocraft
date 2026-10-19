#version 330 core
#ifndef PC_FX_KERNEL
#define PC_FX_KERNEL 0
#endif
uniform sampler2D inputA,inputB,shapeTex,lutTex,patternTex;
uniform vec4 p0,p1,p3,p4;
uniform ivec2 origin,size;
uniform int lutSize;
out vec4 color;
float aAt(ivec2 at){return all(greaterThanEqual(at,ivec2(0)))&&all(lessThan(at,size))?texelFetch(inputA,at,0).r:0.0;}
float table(float v){float x=clamp(v,0,1)*float(lutSize-1);int i=int(floor(x));float a=texelFetch(lutTex,ivec2(i,0),0).r,b=texelFetch(lutTex,ivec2(min(i+1,lutSize-1),0),0).r;return a+(b-a)*(x-float(i));}
float bevelPart(ivec2 at,float depth,float region){
    float gx=(aAt(at+ivec2(1,0))-aAt(at-ivec2(1,0)))*.5*depth;
    float gy=(aAt(at+ivec2(0,1))-aAt(at-ivec2(0,1)))*.5*depth;
    vec3 n=vec3(-gx,-gy,1);float shade=dot(n,p0.xyz)/sqrt(n.x*n.x+n.y*n.y+1),k=shade-p0.w,v=0;
    if(p1.z>.5){if(k<=0)v=clamp(-k/max(p0.w,1e-3),0,1)*region;}
    else if(k>0)v=clamp(k/max(1-p0.w,1e-3),0,1)*region;
    return p1.w>.5?table(v):v;
}
void main(){
    ivec2 at=ivec2(gl_FragCoord.xy);float v=0;
    if(PC_FX_KERNEL==0){
        ivec2 pos=at-ivec2(p0.xy);
        if(any(lessThan(pos,ivec2(0)))||any(greaterThanEqual(pos,size)))v=p0.z;
        else {v=texelFetch(inputA,pos,0).r;if(p0.w>.5)v=1-v;}
    }
    if(PC_FX_KERNEL==1){
        float a=texelFetch(inputA,at,0).r,d=texelFetch(inputB,at,0).r;
        v=d<0?min(a+p0.x,1):max(a,clamp(p0.x+.5-d,0,1));
    }
    if(PC_FX_KERNEL==2){
        int radius=int(clamp(p0.y,0,512));ivec2 step=p0.x>.5?ivec2(0,1):ivec2(1,0);
        for(int k=0;k<=2*radius;k++)v+=aAt(at+step*(k-radius))*texelFetch(lutTex,ivec2(k,0),0).r;
    }
    if(PC_FX_KERNEL==3){
        float d=texelFetch(inputA,at,0).r+.5;v=d<=p0.x?1:.5*clamp(1-(d-p0.x)/p0.y,0,1);
    }
    if(PC_FX_KERNEL==4){
        v=texelFetch(inputA,at,0).r;
        if(p0.x>.5)v=abs(v-texelFetch(inputB,at,0).r);
        if(p0.y>.5&&p0.y<1.5)v=1-v;
        if(p0.z>.5)v=table(v);
        if(p0.y>1.5)v=1-v;
        if(p1.x>0&&v>0){
            uvec2 pos=uvec2(at)+uvec2(origin);uint h=(pos.x*0x8da6b343u)^(pos.y*0xd8163841u)^0x9e3779b9u;
            h^=h>>15;h*=0x2c1b3c6du;h^=h>>12;
            v=clamp(v+(float(h&0xffffu)/65536.0-.5)*2*p1.x,0,1);
        }
        if(p0.w>.5)v*=texelFetch(shapeTex,at,0).r;
    }
    if(PC_FX_KERNEL==5){
        float din=texelFetch(inputA,at,0).r,dout=texelFetch(inputB,at,0).r,ramp=p0.y+1;
        bool soft=p0.z>.5;
        if(p0.x<.5){
            if(soft)v=dout>0?1-clamp((dout+.5)/ramp,0,1):(p0.y+2)/ramp;
            else {float top=(p0.y+2)/ramp;v=dout>0?clamp((p0.y+1.5-dout)/ramp,0,top):top;}
        } else if(p0.x<1.5){float halfSize=p0.y/2+.5;v=din>0?.5+.5*clamp(din/halfSize,0,1):.5-.5*clamp(dout/halfSize,0,1);}
        else if(soft)v=din>0?clamp((din+1.5)/ramp,0,(p0.y+2)/ramp):0;
        else v=din>0?clamp((din+.5)/ramp,0,1):0;
    }
    if(PC_FX_KERNEL==6){
        float s=texelFetch(shapeTex,at,0).r,hmax=aAt(at);
        if(p1.y>1.5||p3.x>.5)hmax=max(max(hmax,max(aAt(at+ivec2(1,0)),aAt(at-ivec2(1,0)))),max(aAt(at+ivec2(0,1)),aAt(at-ivec2(0,1))));
        float outside=s<1-.5/255&&hmax>1e-5?1.0:0.0;
        if(p1.y<.5)v=bevelPart(at,p1.x,s);
        else if(p1.y<1.5)v=bevelPart(at,p1.x,outside);
        else {float inner=s>.5/255?bevelPart(at,p1.x,s):0;v=inner+bevelPart(at,p1.y>2.5?-p1.x:p1.x,outside)*(1-min(s,1));}
    }
    if(PC_FX_KERNEL==7)v=clamp(p0.x+.5-texelFetch(inputA,at,0).r,0,1);
    if(PC_FX_KERNEL==8){
        vec2 point=vec2(at+origin)+.5-p3.xy;
        vec2 tile=vec2(p3.z*point.x-p3.w*point.y,p3.w*point.x+p3.z*point.y)*p4.x;
        vec4 pixel=texture(patternTex,tile/p4.yz);vec3 rgb=pixel.a>0?pixel.rgb/pixel.a:vec3(0);
        float l=dot(rgb,vec3(.299,.587,.114))*pixel.a;if(p0.y>.5)l=1-l;
        v=texelFetch(inputA,at,0).r+p0.x*l;
    }
    color=vec4(v,0,0,1);
}
