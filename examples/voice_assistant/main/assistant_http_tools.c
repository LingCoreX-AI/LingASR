#include "assistant_http_tools.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const cJSON *get(const cJSON *j,const char *k){return cJSON_GetObjectItemCaseSensitive(j,k);}
static const char *text(const cJSON *j){return cJSON_IsString(j)?j->valuestring:NULL;}
static bool fail(char *e,size_t n,const char *s){assistant_copy_text(e,n,s);return false;}
static bool name_valid(const char *s){
    if(!s || !*s || strlen(s)>48 || (!isalpha((unsigned char)*s) && *s!='_'))return false;
    for(;*s;s++)if(!isalnum((unsigned char)*s) && *s!='_' && *s!='-')return false;
    return true;
}
static bool builtin(const char *s){return !strcmp(s,"rgb_control") || !strcmp(s,"web_search") || !strcmp(s,"memory_update");}
static bool url_valid(const char *s){
    if(!s || strlen(s)>384 || strchr(s,'@') || strchr(s,'#'))return false;
    size_t skip=!strncmp(s,"http://",7)?7:!strncmp(s,"https://",8)?8:0;
    if(!skip || !s[skip] || s[skip]=='/' || s[skip]=='?')return false;
    for(;*s;s++)if((unsigned char)*s<=32 || (unsigned char)*s==127 || *s=='\\')return false;
    return true;
}
static bool type_match(const char *type,const cJSON *v){
    if(!strcmp(type,"object"))return cJSON_IsObject(v);
    if(!strcmp(type,"array"))return cJSON_IsArray(v);
    if(!strcmp(type,"string"))return cJSON_IsString(v);
    if(!strcmp(type,"boolean"))return cJSON_IsBool(v);
    if(!strcmp(type,"number"))return cJSON_IsNumber(v) && isfinite(v->valuedouble);
    if(!strcmp(type,"integer"))return cJSON_IsNumber(v) && isfinite(v->valuedouble) && floor(v->valuedouble)==v->valuedouble;
    return false;
}
static bool schema(cJSON *s,unsigned depth){
    const char *type=text(get(s,"type"));if(!cJSON_IsObject(s) || !type || depth>4)return false;
    const cJSON *p;
    cJSON_ArrayForEach(p,s){
        char key[48];if(strlen(p->string)>40)return false;snprintf(key,sizeof(key),"|%s|",p->string);
        if(!strstr("|type|properties|required|additionalProperties|items|enum|minimum|maximum|minLength|maxLength|minItems|maxItems|description|title|default|",key))return false;
    }
    if(!strcmp(type,"object")){
        cJSON *props=cJSON_GetObjectItemCaseSensitive(s,"properties");
        if(!props)props=cJSON_AddObjectToObject(s,"properties");
        if(!cJSON_IsObject(props) || cJSON_GetArraySize(props)>16)return false;
        cJSON_ArrayForEach(p,props)if(!name_valid(p->string) || !schema((cJSON *)p,depth+1))return false;
        const cJSON *required=get(s,"required"),*r;
        if(required && !cJSON_IsArray(required))return false;
        cJSON_ArrayForEach(r,required)if(!text(r) || !get(props,text(r)))return false;
        const cJSON *extra=get(s,"additionalProperties");
        if(!extra)cJSON_AddBoolToObject(s,"additionalProperties",false);else if(!cJSON_IsBool(extra))return false;
    }else if(!strcmp(type,"array")){
        if(!schema(cJSON_GetObjectItemCaseSensitive(s,"items"),depth+1))return false;
    }else if(strcmp(type,"string") && strcmp(type,"boolean") && strcmp(type,"integer") && strcmp(type,"number"))return false;
    const cJSON *values=get(s,"enum");if(values){
        if(!cJSON_IsArray(values) || !cJSON_GetArraySize(values) || cJSON_GetArraySize(values)>16)return false;
        cJSON_ArrayForEach(p,values)if(!type_match(type,p))return false;
    }
    const char *bounds[]={"minimum","maximum","minLength","maxLength","minItems","maxItems"};
    for(unsigned i=0;i<6;i++)if(get(s,bounds[i]) && (!cJSON_IsNumber(get(s,bounds[i])) || !isfinite(get(s,bounds[i])->valuedouble)))return false;
    for(unsigned i=2;i<6;i++)if(get(s,bounds[i]) && (get(s,bounds[i])->valuedouble<0 || floor(get(s,bounds[i])->valuedouble)!=get(s,bounds[i])->valuedouble))return false;
    for(unsigned i=0;i<6;i+=2)if(get(s,bounds[i]) && get(s,bounds[i+1]) && get(s,bounds[i])->valuedouble>get(s,bounds[i+1])->valuedouble)return false;
    return true;
}
static bool arguments(const cJSON *s,const cJSON *v,unsigned depth){
    const char *type=text(get(s,"type"));if(!type || depth>4 || !type_match(type,v))return false;
    const cJSON *values=get(s,"enum"),*p;bool found=!values;
    cJSON_ArrayForEach(p,values)found|=cJSON_Compare(p,v,true);
    if(!found)return false;
    double length=0;
    if(!strcmp(type,"object")){
        const cJSON *props=get(s,"properties"),*r;
        cJSON_ArrayForEach(r,get(s,"required"))if(!get(v,text(r)))return false;
        cJSON_ArrayForEach(p,v){const cJSON *spec=get(props,p->string);
            if(spec){if(!arguments(spec,p,depth+1))return false;}else if(!cJSON_IsTrue(get(s,"additionalProperties")))return false;
        }
    }else if(!strcmp(type,"array")){
        length=cJSON_GetArraySize(v);if(length>32)return false;
        cJSON_ArrayForEach(p,v)if(!arguments(get(s,"items"),p,depth+1))return false;
    }else if(!strcmp(type,"string")){
        for(const unsigned char *c=(const unsigned char *)v->valuestring;*c;c++)length+=(*c&0xc0)!=0x80;
        if(strlen(v->valuestring)>1536)return false;
    }else if(cJSON_IsNumber(v)){
        if(get(s,"minimum") && v->valuedouble<get(s,"minimum")->valuedouble)return false;
        if(get(s,"maximum") && v->valuedouble>get(s,"maximum")->valuedouble)return false;
    }
    const char *lo=!strcmp(type,"array")?"minItems":"minLength",*hi=!strcmp(type,"array")?"maxItems":"maxLength";
    if((!strcmp(type,"array") || !strcmp(type,"string")) && ((get(s,lo) && length<get(s,lo)->valuedouble) || (get(s,hi) && length>get(s,hi)->valuedouble)))return false;
    return true;
}
cJSON *assistant_tools_default(const assistant_options_t *o){
    cJSON *j=cJSON_CreateObject(),*b=cJSON_AddObjectToObject(j,"builtin"),*s=cJSON_AddObjectToObject(j,"settings");
    cJSON_AddBoolToObject(b,"rgb_control",true);cJSON_AddBoolToObject(b,"web_search",true);cJSON_AddBoolToObject(b,"memory_update",true);
    cJSON_AddNumberToObject(s,"search_provider",o->search_provider);cJSON_AddStringToObject(s,"search_model",o->search_model);cJSON_AddNumberToObject(s,"rgb_gpio",o->rgb_gpio);
    cJSON_AddArrayToObject(j,"custom");return j;
}
const cJSON *assistant_tools_find(const cJSON *config,const char *name){
    const cJSON *p;cJSON_ArrayForEach(p,get(config,"custom"))if(text(get(p,"name")) && !strcmp(text(get(p,"name")),name))return p;
    return NULL;
}
bool assistant_tools_enabled(const cJSON *config,const char *name){
    if(builtin(name))return !config || cJSON_IsTrue(get(get(config,"builtin"),name));
    const cJSON *p=assistant_tools_find(config,name);return p && cJSON_IsTrue(get(p,"enabled"));
}
cJSON *assistant_tools_normalize(const cJSON *input,const cJSON *previous,char *error,size_t n){
    if(!cJSON_IsObject(input)){fail(error,n,"工具配置必须是JSON对象");return NULL;}
    assistant_options_t options;assistant_options_default(&options);
    if(previous && !assistant_options_parse(get(previous,"settings"),&options,&options)){fail(error,n,"已有工具设置无效");return NULL;}
    cJSON *out=previous?cJSON_Duplicate(previous,true):assistant_tools_default(&options);
    if(!out){fail(error,n,"工具配置内存不足");return NULL;}
    const cJSON *b=get(input,"builtin"),*p,*s=get(input,"settings"),*list=get(input,"custom");
    if(b){if(!cJSON_IsObject(b))goto invalid;
        cJSON_ArrayForEach(p,b){if(!builtin(p->string) || !cJSON_IsBool(p))goto invalid;cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(out,"builtin"),p->string,cJSON_CreateBool(cJSON_IsTrue(p)));}
    }
    if(s){
        if(!cJSON_IsObject(s) || !assistant_options_parse(s,&options,&options))goto invalid;
        cJSON_ArrayForEach(p,s)if(strcmp(p->string,"search_provider") && strcmp(p->string,"search_model") && strcmp(p->string,"rgb_gpio"))goto invalid;
        cJSON *settings=cJSON_CreateObject();cJSON_AddNumberToObject(settings,"search_provider",options.search_provider);cJSON_AddStringToObject(settings,"search_model",options.search_model);cJSON_AddNumberToObject(settings,"rgb_gpio",options.rgb_gpio);
        cJSON_ReplaceItemInObjectCaseSensitive(out,"settings",settings);
    }
    if(list){
        if(!cJSON_IsArray(list) || cJSON_GetArraySize(list)>ASSISTANT_HTTP_TOOLS)goto invalid;
        cJSON *custom=cJSON_CreateArray();cJSON_ReplaceItemInObjectCaseSensitive(out,"custom",custom);
        cJSON_ArrayForEach(p,list){
            const char *name=text(get(p,"name")),*desc=text(get(p,"description")),*url=text(get(p,"url")),*method=text(get(p,"method"));
            if(!name_valid(name) || builtin(name) || assistant_tools_find(out,name) || !desc || !*desc || strlen(desc)>256 || !url_valid(url) || !method || (strcmp(method,"GET") && strcmp(method,"POST")) || (get(p,"enabled") && !cJSON_IsBool(get(p,"enabled"))))goto invalid;
            cJSON *params=cJSON_Duplicate(get(p,"parameters"),true);
            if(!params || !text(get(params,"type")) || strcmp(text(get(params,"type")),"object") || !schema(params,0)){cJSON_Delete(params);goto invalid;}
            char *encoded=cJSON_PrintUnformatted(params);bool size_ok=encoded && strlen(encoded)<=1024;free(encoded);
            if(!size_ok){cJSON_Delete(params);goto invalid;}
            cJSON *t=cJSON_CreateObject();cJSON_AddStringToObject(t,"name",name);cJSON_AddStringToObject(t,"description",desc);cJSON_AddStringToObject(t,"url",url);cJSON_AddStringToObject(t,"method",method);
            cJSON_AddBoolToObject(t,"enabled",!cJSON_IsFalse(get(p,"enabled")));cJSON_AddItemToObject(t,"parameters",params);cJSON_AddItemToArray(custom,t);
        }
    }
    char *encoded=cJSON_PrintUnformatted(out);bool size_ok=encoded && strlen(encoded)<ASSISTANT_TOOL_CONFIG_BYTES;free(encoded);
    if(size_ok)return out;
invalid:
    cJSON_Delete(out);fail(error,n,"工具定义无效或过长：最多4个工具；名称需唯一，支持GET/POST及对象类型的JSON Schema参数");return NULL;
}
cJSON *assistant_tools_schemas(const cJSON *config){
    cJSON *out=cJSON_CreateArray();const cJSON *p;
    if(!out)return NULL;
    cJSON_ArrayForEach(p,get(config,"custom"))if(cJSON_IsTrue(get(p,"enabled"))){
        cJSON *t=cJSON_CreateObject(),*f=cJSON_AddObjectToObject(t,"function");cJSON_AddStringToObject(t,"type","function");
        cJSON_AddStringToObject(f,"name",text(get(p,"name")));cJSON_AddStringToObject(f,"description",text(get(p,"description")));cJSON_AddItemToObject(f,"parameters",cJSON_Duplicate(get(p,"parameters"),true));cJSON_AddItemToArray(out,t);
    }
    return out;
}
bool assistant_tools_arguments(const cJSON *def,const cJSON *args,char *e,size_t n){
    return arguments(get(def,"parameters"),args,0) || fail(e,n,"工具参数不符合已保存的JSON Schema");
}
static bool append_encoded(char *url,size_t size,size_t *used,const char *value){
    for(const unsigned char *p=(const unsigned char *)value;*p;p++){
        bool plain=isalnum(*p) || strchr("-_.~",*p);size_t need=plain?1:3;if(*used+need>=size)return false;
        if(plain)url[(*used)++]=(char)*p;else{snprintf(url+*used,size-*used,"%%%02X",*p);*used+=3;}
    }
    url[*used]=0;return true;
}
bool assistant_tools_http_request(const cJSON *def,const cJSON *args,char *url,size_t size,char **body,char *e,size_t n){
    *body=NULL;if(!assistant_tools_arguments(def,args,e,n))return false;
    const char *base=text(get(def,"url")),*method=text(get(def,"method"));
    if(!url_valid(base) || strlen(base)>=size || !method || (strcmp(method,"GET") && strcmp(method,"POST")))return fail(e,n,"工具接口地址或请求方式无效");
    strcpy(url,base);size_t used=strlen(url);
    if(!strcmp(method,"POST")){*body=cJSON_PrintUnformatted(args);return *body!=NULL;}
    const cJSON *p;cJSON_ArrayForEach(p,args){
        char *value=cJSON_IsString(p)?NULL:cJSON_PrintUnformatted(p);const char *s=cJSON_IsString(p)?p->valuestring:value;
        if(!s || used+2>=size){free(value);return fail(e,n,"GET参数过长，请改用POST");}
        url[used++]=strchr(url,'?')?'&':'?';url[used]=0;
        bool ok=append_encoded(url,size,&used,p->string);if(ok && used+1<size){url[used++]='=';url[used]=0;ok=append_encoded(url,size,&used,s);}else ok=false;
        free(value);if(!ok)return fail(e,n,"GET参数过长，请改用POST");
    }
    return true;
}
