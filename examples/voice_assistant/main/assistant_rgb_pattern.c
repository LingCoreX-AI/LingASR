#include "assistant_rgb_pattern.h"
uint32_t assistant_rgb_frame(const assistant_rgb_command_t *c,unsigned elapsed,unsigned *r,unsigned *g,unsigned *b)
{
    static const unsigned colors[6][3]={{255,0,0},{0,255,0},{0,0,255},{255,255,0},{0,255,255},{255,0,255}};
    *r=c->red;*g=c->green;*b=c->blue;
    if(c->mode==RGB_OFF || (c->duration_ms && elapsed>=c->duration_ms)){*r=*g=*b=0;}
    else if(c->mode>=RGB_BLINK && !c->period_ms){*r=*g=*b=0;}
    else if(c->mode==RGB_CYCLE || c->mode==RGB_CYCLE_BLINK){
        unsigned i=elapsed/c->period_ms%6;*r=colors[i][0];*g=colors[i][1];*b=colors[i][2];
        if(c->mode==RGB_CYCLE_BLINK && elapsed%c->period_ms>=c->period_ms/2)*r=*g=*b=0;
    }else if(c->mode==RGB_BLINK && elapsed%c->period_ms>=c->period_ms/2)*r=*g=*b=0;
    return (*r<<16)|(*g<<8)|*b;
}
