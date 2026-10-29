// Round-trip test of TranscodeBc3BlockToAstc4x4 and TranscodeBc5BlockToAstc4x4 against the ARM astc-encoder reference decoder.
// Build: g++ -O2 -std=c++17 astc_bc3_test.cpp -I<astc-encoder>/Source <astc-encoder>/build/Source/libastcenc-none-static.a -lpthread
// Expected: no undecodable block, DXT colour error at most 4/255, cut-out alpha exact, DXN channels read back as R=G=B (luminance), model within 1.
#include "../../../glue/rexglue-sdk-main/src/graphics/gta4_native/native_astc_transcode.h"
#include "astcenc.h"
#include <cstdio>
#include <random>
using namespace rex::graphics::gta4_native;
static void RefColour(const uint8_t* b, uint8_t out[16][3]) {
  uint32_t c0=b[0]|(b[1]<<8), c1=b[2]|(b[3]<<8); int p[4][3]; int e0[3], e1[3];
  astc_detail::Expand565(c0,e0); astc_detail::Expand565(c1,e1);
  for(int k=0;k<3;++k){p[0][k]=e0[k];p[1][k]=e1[k];p[2][k]=(2*e0[k]+e1[k]+1)/3;p[3][k]=(e0[k]+2*e1[k]+1)/3;}
  uint32_t sel=b[4]|(b[5]<<8)|(b[6]<<16)|((uint32_t)b[7]<<24);
  for(int i=0;i<16;++i)for(int k=0;k<3;++k)out[i][k]=p[(sel>>(2*i))&3][k];
}
static void RefAlpha(const uint8_t* block,bool expl,int alpha[16]){
  if(expl){for(int i=0;i<16;++i)alpha[i]=((block[i>>1]>>((i&1)*4))&15)*17;return;}
  int pal[8]={block[0],block[1]};
  if(block[0]>block[1]){for(int k=2;k<8;++k)pal[k]=((8-k)*block[0]+(k-1)*block[1])/7;}
  else{for(int k=2;k<6;++k)pal[k]=((6-k)*block[0]+(k-1)*block[1])/5;pal[6]=0;pal[7]=255;}
  uint64_t s=0;for(int b=0;b<6;++b)s|=uint64_t(block[b+2])<<(b*8);
  for(int i=0;i<16;++i)alpha[i]=pal[(s>>(i*3))&7];
}
int main(){
  astcenc_config cfg; astcenc_config_init(ASTCENC_PRF_LDR,4,4,1,ASTCENC_PRE_FASTEST,ASTCENC_FLG_DECOMPRESS_ONLY,&cfg);
  astcenc_context* ctx; if(astcenc_context_alloc(&cfg,1,&ctx,nullptr)){puts("ctx");return 1;}
  std::mt19937 rng(7);
  // DXN: two BC4 blocks; smooth (a normal map: small ranges) and random data
  for(int kind=0;kind<3;++kind){
    long n=200000; int maxe=0; double sq=0; long over16=0; long maxlum=0, modelbad=0;
    for(long it=0;it<n;++it){
      uint8_t b[16]; for(auto&x:b)x=rng()&255;
      if(kind==1){ for(int ch=0;ch<2;++ch){ int base=100+rng()%56; int range=rng()%24; b[ch*8]=base+range; b[ch*8+1]=base; } }
      if(kind==2){ for(int ch=0;ch<2;++ch){ int base=rng()%200; int range=rng()%100; b[ch*8]=base+range; b[ch*8+1]=base; } }
      uint8_t astc[16]; AstcAlphaFit fit=TranscodeBc5BlockToAstc4x4(b,b+8,astc);
      uint8_t out[64]; astcenc_image img{}; img.dim_x=4;img.dim_y=4;img.dim_z=1;img.data_type=ASTCENC_TYPE_U8; void* sl=out; img.data=&sl;
      astcenc_swizzle sw{ASTCENC_SWZ_R,ASTCENC_SWZ_G,ASTCENC_SWZ_B,ASTCENC_SWZ_A};
      if(astcenc_decompress_image(ctx,astc,16,&img,&sw,0)){puts("undecodable");continue;}
      astcenc_decompress_reset(ctx);
      int r0[16],r1[16]; DecodeBc4Block(b,r0); DecodeBc4Block(b+8,r1);
      int bm=0; for(int i=0;i<16;++i){ int e0=abs(int(out[i*4])-r0[i]), e1=abs(int(out[i*4+3])-r1[i]); bm=std::max(bm,std::max(e0,e1)); sq+=e0*e0+e1*e1; if(out[i*4]!=out[i*4+1]||out[i*4]!=out[i*4+2]) maxlum++; }
      maxe=std::max(maxe,bm); if(bm>16)over16++;
      if(abs(bm-int(fit.max_error))>1) modelbad++;
    }
    printf("DXN %s: max error %d, rms %.2f, blocks error>16: %ld/%ld, (texels where R!=G!=B: %ld, blocks model differs by >1: %ld)\n",kind==0?"random":kind==1?"normal-like":"wide-range",maxe,sqrt(sq/(n*32.0)),over16,n,maxlum,modelbad);
  }
  for(int mode=0;mode<6;++mode){
    long bad=0,undec=0; int maxc=0,maxa=0; double sqa=0; long n=200000; long fitmax_gt24=0;
    for(long it=0;it<n;++it){
      uint8_t b[16]; for(auto&x:b)x=rng()&255;
      bool expl=mode&1; int kind=mode>>1;
      if(kind==1){ // cutout: dxt5 a0=255 a1=0 selectors only 0/1; dxt3 0 or 15
        if(expl){for(int i=0;i<8;++i){int v=0;for(int h=0;h<2;++h)v|=((rng()&1)?15:0)<<(4*h);b[i]=v;}}
        else{b[0]=255;b[1]=0;uint64_t s=0;for(int i=0;i<16;++i)s|=uint64_t(rng()&1)<<(i*3);for(int k=0;k<6;++k)b[2+k]=(s>>(8*k))&255;}
      } else if(kind==2){ // smooth alpha: two close endpoints
        if(!expl){int a=rng()&255;b[0]=a;b[1]=std::max(0,a-int(rng()%60));}
      }
      uint8_t astc[16]; AstcAlphaFit fit=TranscodeBc3BlockToAstc4x4(b,expl,astc);
      uint8_t out[64]; astcenc_image img{}; img.dim_x=4;img.dim_y=4;img.dim_z=1;img.data_type=ASTCENC_TYPE_U8; void* sl=out; img.data=&sl;
      astcenc_swizzle sw{ASTCENC_SWZ_R,ASTCENC_SWZ_G,ASTCENC_SWZ_B,ASTCENC_SWZ_A};
      if(astcenc_decompress_image(ctx,astc,16,&img,&sw,0)){undec++;continue;}
      astcenc_decompress_reset(ctx);
      uint8_t col[16][3]; int al[16]; RefColour(b+8,col); RefAlpha(b,expl,al);
      int bc=0,ba=0;
      for(int i=0;i<16;++i){for(int k=0;k<3;++k)bc=std::max(bc,std::abs(int(out[i*4+k])-col[i][k]));ba=std::max(ba,std::abs(int(out[i*4+3])-al[i]));sqa+=(out[i*4+3]-al[i])*(out[i*4+3]-al[i]);}
      maxc=std::max(maxc,bc);maxa=std::max(maxa,ba); if(ba>24)fitmax_gt24++;
      if(ba!=int(fit.max_error)&&ba>int(fit.max_error)+2)bad++;
    }
    printf("mode %d (%s, %s): undecodable %ld, max colour err %d, max alpha err %d, rms alpha %.2f, blocks alpha>24: %ld/%ld, model-mismatch %ld\n",mode,mode&1?"DXT3":"DXT5",mode>>1==0?"random":mode>>1==1?"cutout":"smooth",undec,maxc,maxa,sqrt(sqa/(n*16.0)),fitmax_gt24,n,bad);
  }
}
