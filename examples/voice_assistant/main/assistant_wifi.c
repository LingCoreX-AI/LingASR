#include "assistant_wifi.h"
#include <ctype.h>
#include <string.h>

static bool password_valid(const char *p)
{
    size_t n=strlen(p);if(!n || (n>=8 && n<=63))return true;
    if(n!=64)return false;
    for(size_t i=0;i<n;i++)if(!isxdigit((unsigned char)p[i]))return false;
    return true;
}
void assistant_wifi_init(assistant_wifi_store_t *s) { memset(s,0,sizeof(*s));s->version=1; }
int assistant_wifi_find(const assistant_wifi_store_t *s,const char *ssid)
{
    for(unsigned i=0;i<s->count;i++)if(!strcmp(s->profiles[i].ssid,ssid))return (int)i;
    return -1;
}
bool assistant_wifi_valid(const assistant_wifi_store_t *s)
{
    if(s->version!=1 || s->count>ASSISTANT_WIFI_MAX || !memchr(s->preferred,0,sizeof(s->preferred)))return false;
    for(unsigned i=0;i<s->count;i++) {
        const assistant_wifi_profile_t *p=s->profiles+i;
        if(!p->ssid[0] || !memchr(p->ssid,0,sizeof(p->ssid)) || !memchr(p->password,0,sizeof(p->password)) || p->hidden>1 || !password_valid(p->password))return false;
        for(unsigned j=0;j<i;j++)if(!strcmp(p->ssid,s->profiles[j].ssid))return false;
    }
    return (!s->count && !s->preferred[0]) || assistant_wifi_find(s,s->preferred)>=0;
}
bool assistant_wifi_save(assistant_wifi_store_t *s,const char *ssid,const char *password,bool hidden)
{
    if(!ssid[0] || strlen(ssid)>32 || !password_valid(password))return false;
    int i=assistant_wifi_find(s,ssid);
    if(i<0) { if(s->count==ASSISTANT_WIFI_MAX)return false;i=(int)s->count++; }
    assistant_wifi_profile_t *p=s->profiles+i;memset(p,0,sizeof(*p));
    memcpy(p->ssid,ssid,strlen(ssid)+1);memcpy(p->password,password,strlen(password)+1);p->hidden=hidden;
    memcpy(s->preferred,ssid,strlen(ssid)+1);return true;
}
bool assistant_wifi_remove(assistant_wifi_store_t *s,const char *ssid)
{
    int i=assistant_wifi_find(s,ssid);if(i<0)return false;
    memmove(s->profiles+i,s->profiles+i+1,(s->count-(unsigned)i-1)*sizeof(s->profiles[0]));
    memset(s->profiles+--s->count,0,sizeof(s->profiles[0]));
    if(!strcmp(s->preferred,ssid)) { memset(s->preferred,0,sizeof(s->preferred));if(s->count)memcpy(s->preferred,s->profiles[0].ssid,sizeof(s->preferred)); }
    return true;
}
int assistant_wifi_choose(const assistant_wifi_store_t *s,const assistant_wifi_ap_t *aps,size_t count,unsigned tried)
{
    int best=-1,signal=-1000;
    for(unsigned i=0;i<s->count;i++) {
        if(tried&(1u<<i))continue;
        for(size_t a=0;a<count;a++)if(!strcmp(s->profiles[i].ssid,aps[a].ssid)) {
            if(!strcmp(s->preferred,s->profiles[i].ssid))return (int)i;
            if(aps[a].rssi>signal){signal=aps[a].rssi;best=(int)i;}
        }
    }
    if(best>=0)return best;
    for(unsigned i=0;i<s->count;i++)if(!(tried&(1u<<i)) && s->profiles[i].hidden)return (int)i;
    /* Legacy single-network settings carry no hidden-network metadata.
     * Try the preferred SSID once by name after visible candidates. */
    int preferred=assistant_wifi_find(s,s->preferred);
    if(preferred>=0 && !(tried&(1u<<preferred)))return preferred;
    return -1;
}
