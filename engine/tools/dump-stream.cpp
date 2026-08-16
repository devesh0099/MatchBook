// dump-stream.cpp - print every event of a gen stream as text.
//   dump-stream FILE            (all events)
// Build: g++ -std=c++20 -I engine/include engine/tools/dump-stream.cpp -o dump-stream
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "mebench/wire.h"
#include "mebench/order.h"
using namespace mebench;
static const char* side_s(Side s){ return s==Side::Buy?"buy":"sell"; }
static const char* tif_s(TIF t){
  switch(t){case TIF::GTC:return "GTC";case TIF::IOC:return "IOC";
            case TIF::FOK:return "FOK";case TIF::Market:return "Market";} return "?";
}
int main(int argc, char** argv){
  if(argc<2){ fprintf(stderr,"usage: dump-stream FILE\n"); return 2; }
  FILE* f=fopen(argv[1],"rb"); if(!f){ perror("open"); return 2; }
  StreamHeader h;
  if(fread(&h,sizeof h,1,f)!=1 || memcmp(h.magic,kStreamMagic,8)!=0){ fprintf(stderr,"not a gen stream\n"); return 2; }
  fprintf(stderr,"# seed=%llu events=%llu profile=%u\n",
          (unsigned long long)h.seed,(unsigned long long)h.event_count,h.profile_id);
  WireEvent e; uint64_t seq=0;
  while(fread(&e,sizeof e,1,f)==1){
    if(e.type==EvType::Cancel)
      printf("seq=%llu CANCEL session=%u coid=%llu firm=%u\n",
             (unsigned long long)seq,e.session_id,(unsigned long long)e.client_order_id,e.participant_id);
    else
      printf("seq=%llu NEW %s %u @ %d session=%u coid=%llu firm=%u tif=%s\n",
             (unsigned long long)seq,side_s(e.side),e.qty,e.price,e.session_id,
             (unsigned long long)e.client_order_id,e.participant_id,tif_s(e.tif));
    ++seq;
  }
  fclose(f); return 0;
}
