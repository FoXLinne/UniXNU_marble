#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
typedef uint32_t uint2 __attribute__((ext_vector_type(2)));
extern void scanout_scale(const uint32_t *, uint32_t *, const uint32_t *, const uint32_t *, uint2);
static uint32_t code(const uint32_t *s, const uint32_t *t, unsigned stride, unsigned x, unsigned y, unsigned format) {
    if (format == 1) return s[y*stride+x] | 0xff000000u;
    const uint16_t *p = (const uint16_t *)(s+y*stride+x*2);
    return 0xff000000u | (t[p[0]]<<16) | (t[p[1]]<<8) | t[p[2]];
}
static void trial(unsigned sw,unsigned sh,unsigned dw,unsigned dh,unsigned ox,unsigned oy,unsigned format) {
    const unsigned stride=(sw+ox+5)*(format==1?1:2), ds=dw+7;
    uint32_t *src=calloc(stride*(sh+oy+2),4), *out=malloc(ds*(dh+1)*4), *table=malloc(65536*4);
    assert(src && out && table);
    for (unsigned i=0;i<65536;i++) table[i]=i&255;
    for (unsigned y=0;y<sh+oy+2;y++) for(unsigned x=0;x<sw+ox+5;x++) {
        uint32_t r=(x*13+y*7)&255,g=(x*3+y*17)&255,b=(x*19+y*5)&255;
        if (format==1) src[y*stride+x]=(r<<16)|(g<<8)|b;
        else {src[y*stride+x*2]=r|(g<<16);src[y*stride+x*2+1]=b|0x3c000000;}
    }
    for (unsigned i=0;i<ds*(dh+1);i++) out[i]=0x5a5a5a5a;
    const uint32_t p[12]={sw,sh,stride,ds,dw,dh,ox,oy,format,0,0,0};
    for(unsigned y=0;y<dh+2;y++) for(unsigned x=0;x<dw+2;x++) scanout_scale(src,out,table,p,(uint2){x,y});
    unsigned wrong=0;
    for(unsigned y=0;y<dh;y++) for(unsigned x=0;x<dw;x++) {
        float sx=fminf(sw-1,fmaxf(0,((float)x+.5f)*sw/dw-.5f));
        float sy=fminf(sh-1,fmaxf(0,((float)y+.5f)*sh/dh-.5f));
        unsigned x0=(unsigned)sx,y0=(unsigned)sy,x1=x0+1<sw?x0+1:x0,y1=y0+1<sh?y0+1:y0;
        float fx=sx-x0,fy=sy-y0;
        uint32_t c[4]={code(src,table,stride,x0+ox,y0+oy,format),code(src,table,stride,x1+ox,y0+oy,format),code(src,table,stride,x0+ox,y1+oy,format),code(src,table,stride,x1+ox,y1+oy,format)};
        for(unsigned k=0;k<3;k++) {
            float v[4];for(unsigned i=0;i<4;i++)v[i]=(c[i]>>(8*k))&255;
            float top=v[0]+(v[1]-v[0])*fx,bot=v[2]+(v[3]-v[2])*fx;
            int want=(int)(top+(bot-top)*fy+.5f), got=(out[y*ds+x]>>(8*k))&255;
            if(abs(want-got)>1) wrong++;
        }
        assert(out[y*ds+x]>>24==255);
    }
    for(unsigned y=0;y<dh;y++) for(unsigned x=dw;x<ds;x++) assert(out[y*ds+x]==0x5a5a5a5a);
    for(unsigned x=0;x<ds;x++) assert(out[dh*ds+x]==0x5a5a5a5a);
    assert(wrong==0);free(src);free(out);free(table);
}
int main(void) {
    for(unsigned f=1;f<=3;f++) {trial(1,1,13,15,0,0,f);trial(13,9,19,17,2,1,f);trial(17,11,7,5,1,3,f);}
    trial(880,1956,1080,2400,0,0,1);
    puts("PASS production AIR: BGRA and half-table interpolation, crop offsets, edges, padded strides, output bounds and 880x1956 to 1080x2400");
}
