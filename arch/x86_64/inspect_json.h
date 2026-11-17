/* Strict spans for the few JSON fields used by the native DevTools diagnostic. */
#ifndef NEXUS_INSPECT_JSON_H
#define NEXUS_INSPECT_JSON_H
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
struct json_span { const char *begin, *end; };
static void json_space(const char** p,const char* end) { while(*p<end&&(**p==' '||**p=='\t'||**p=='\r'||**p=='\n'))(*p)++; }
static bool json_string(const char** p,const char* end,struct json_span* text) {
    if(*p==end||*(*p)++!='"')return false;
    const char* begin=*p;
    while(*p<end) {
        unsigned char c=*(*p)++;
        if(c=='"'){if(text)*text=(struct json_span){begin,*p-1};return true;}
        if(c<32)return false;
        if(c=='\\') {
            if(*p==end)return false;
            c=*(*p)++;
            if(c=='u'){for(unsigned i=0;i<4;i++)if(*p==end||!isxdigit((unsigned char)*(*p)++))return false;}
            else if(!strchr("\"\\/bfnrt",c))return false;
        }
    }
    return false;
}
static bool json_skip(const char** p,const char* end,unsigned depth) {
    json_space(p,end);if(*p==end||depth>64)return false;
    char c=**p;
    if(c=='"')return json_string(p,end,NULL);
    if(c=='{'||c=='[') {
        char close=c=='{'?'}':']';(*p)++;json_space(p,end);
        if(*p<end&&**p==close){(*p)++;return true;}
        for(;;) {
            if(c=='{'){if(!json_string(p,end,NULL))return false;json_space(p,end);if(*p==end||*(*p)++!=':')return false;}
            if(!json_skip(p,end,depth+1))return false;json_space(p,end);
            if(*p==end)return false;
            if(**p==close){(*p)++;return true;}
            if(*(*p)++!=',')return false;json_space(p,end);
        }
    }
    const char* literal=c=='t'?"true":c=='f'?"false":c=='n'?"null":NULL;
    if(literal){size_t n=strlen(literal);if((size_t)(end-*p)<n||memcmp(*p,literal,n))return false;*p+=n;return true;}
    if(c=='-')(*p)++;
    if(*p==end)return false;
    if(**p=='0')(*p)++;
    else {if(**p<'1'||**p>'9')return false;while(*p<end&&isdigit((unsigned char)**p))(*p)++;}
    if(*p<end&&**p=='.'){(*p)++;if(*p==end||!isdigit((unsigned char)**p))return false;while(*p<end&&isdigit((unsigned char)**p))(*p)++;}
    if(*p<end&&(**p=='e'||**p=='E')){(*p)++;if(*p<end&&(**p=='+'||**p=='-'))(*p)++;if(*p==end||!isdigit((unsigned char)**p))return false;while(*p<end&&isdigit((unsigned char)**p))(*p)++;}
    return true;
}
static bool json_valid(const char* text) {
    const char *p=text,*end=text+strlen(text);if(!json_skip(&p,end,0))return false;json_space(&p,end);return p==end;
}
static struct json_span json_member(struct json_span object,const char* name) {
    const char *p=object.begin,*end=object.end;struct json_span missing={0};size_t size=strlen(name);
    if(!p)return missing;json_space(&p,end);if(p==end||*p++!='{')return missing;json_space(&p,end);
    while(p<end&&*p!='}') {
        struct json_span key;if(!json_string(&p,end,&key))return missing;json_space(&p,end);if(p==end||*p++!=':')return missing;
        json_space(&p,end);const char* value=p;if(!json_skip(&p,end,0))return missing;
        if((size_t)(key.end-key.begin)==size&&!memcmp(key.begin,name,size))return (struct json_span){value,p};
        json_space(&p,end);if(p==end||*p++!=',')return missing;json_space(&p,end);
    }
    return missing;
}
static bool json_equal(struct json_span value,const char* text) {
    if(!value.begin)return false;const char* p=value.begin;struct json_span contents;
    return json_string(&p,value.end,&contents)&&p==value.end&&(size_t)(contents.end-contents.begin)==strlen(text)&&!memcmp(contents.begin,text,strlen(text));
}
static bool json_identifier(struct json_span value,char* result,size_t capacity) {
    if(!value.begin)return false;const char* p=value.begin;struct json_span contents;
    if(!json_string(&p,value.end,&contents)||p!=value.end||contents.begin==contents.end||(size_t)(contents.end-contents.begin)>=capacity)return false;
    size_t n=0;for(const char* c=contents.begin;c<contents.end;c++){if(!isalnum((unsigned char)*c)&&*c!='-'&&*c!='_')return false;result[n++]=*c;}
    result[n]=0;return true;
}
static bool json_id(struct json_span value,unsigned wanted) {
    if(!value.begin||value.begin==value.end)return false;
    unsigned id=0;for(const char* p=value.begin;p<value.end;p++){if(!isdigit((unsigned char)*p)||id>(~0U-(unsigned)(*p-'0'))/10)return false;id=id*10+*p-'0';}
    return id==wanted;
}
#endif
