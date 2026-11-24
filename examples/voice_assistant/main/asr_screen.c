#include "asr_screen.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "asr_lcd_font.h"

enum { CELL=20, COLS=11, ROWS=9, PAGE_CHARS=COLS*ROWS, TOP=12, ROW_STEP=24 };
static const uint16_t BACKGROUND=0x0843, FOREGROUND=0xeffd;

static uint32_t next_char(const char **text)
{
    const unsigned char *p=(const unsigned char *)*text;
    uint32_t c=*p++;
    if (!c) return 0;
    int extra=0;
    uint32_t minimum=0;
    if (c>=0xc2 && c<=0xdf) { c&=31; extra=1; minimum=0x80; }
    else if (c>=0xe0 && c<=0xef) { c&=15; extra=2; minimum=0x800; }
    else if (c>=0xf0 && c<=0xf4) { c&=7; extra=3; minimum=0x10000; }
    else if (c>=0x80) { *text=(const char *)p; return '?'; }
    for (int i=0; i<extra; i++) {
        if ((*p&0xc0)!=0x80) { *text=(const char *)p; return '?'; }
        c=(c<<6)|(*p++&63);
    }
    *text=(const char *)p;
    return c<minimum || c>0x10ffff || (c>=0xd800 && c<=0xdfff) ? '?' : c;
}

bool asr_screen_draft_step(asr_screen_state_t *s,const asr_screen_state_t *previous,bool advance)
{
    if(!s->draft)return false;
    size_t shown=s->draft_head_bytes;
    if(previous->draft && previous->id==s->id){
        size_t match=0;
        while(s->text[match] && s->text[match]==previous->text[match])match++;
        while(((unsigned char)s->text[match]&0xc0)==0x80)match--;
        if(match>shown)shown=match;
    }
    const char *end=s->text+shown;
    if(advance && *end)next_char(&end);
    bool pending=*end!=0;
    shown=end-s->text;
    memset(s->text+shown,0,sizeof(s->text)-shown);
    return pending;
}

static const uint8_t *glyph(uint32_t cp)
{
    size_t lo=0, hi=ASR_FONT_COUNT;
    while (lo<hi) {
        size_t mid=lo+(hi-lo)/2;
        if (asr_font_codes[mid]<cp) lo=mid+1; else hi=mid;
    }
    if (lo<ASR_FONT_COUNT && asr_font_codes[lo]==cp) return asr_font_bits[lo];
    return asr_font_bits[ASR_FONT_FALLBACK];
}

static void draw_char(uint16_t *frame, int x, int y, uint32_t cp, uint16_t color)
{
    const uint8_t *bits=glyph(cp);
    for (int row=0; row<CELL; row++) for (int col=0; col<CELL; col++) {
        int px=x+col, py=y+row;
        if (px>=0 && px<240 && py>=0 && py<240 && (bits[row*3+col/8]&(0x80>>(col%8))))
            frame[py*240+px]=color;
    }
}

static unsigned positions(const char *p)
{
    unsigned position=0;
    uint32_t cp;
    while ((cp=next_char(&p))) {
        if (cp=='\n') position=(position/COLS+1)*COLS;
        else position++;
    }
    return position;
}

unsigned asr_screen_page_count(const asr_screen_state_t *s)
{
    unsigned count=positions(s->text);
    return count ? (count+PAGE_CHARS-1)/PAGE_CHARS : 1;
}

bool asr_screen_append_rows(const asr_screen_state_t *s,const asr_screen_state_t *previous,
                            unsigned page,unsigned *first,unsigned *stop)
{
    if(!s->draft || !previous->draft || s->id!=previous->id)return false;
    size_t prefix=strlen(previous->text);
    if(strncmp(s->text,previous->text,prefix))return false;
    const char *end=s->text+prefix;
    uint32_t added=next_char(&end);
    if(!added || added=='\n' || *end)return false;
    unsigned position=positions(previous->text);
    if(position/PAGE_CHARS!=page)return false;
    *first=TOP+(position%PAGE_CHARS/COLS)*ROW_STEP;
    *stop=*first+CELL;
    return true;
}

unsigned asr_screen_render(uint16_t *frame, const asr_screen_state_t *s, unsigned page)
{
    for (int i=0; i<240*240; i++) frame[i]=BACKGROUND;
    unsigned pages=asr_screen_page_count(s);
    page%=pages;
    unsigned position=0;
    const char *p=s->text;
    uint32_t cp;
    while ((cp=next_char(&p))) {
        if (cp=='\n') { position=(position/COLS+1)*COLS; continue; }
        if (position/PAGE_CHARS==page) {
            unsigned cell=position%PAGE_CHARS;
            draw_char(frame,10+(cell%COLS)*CELL,TOP+(cell/COLS)*ROW_STEP,cp,FOREGROUND);
        }
        position++;
    }
    return pages;
}
