#include "../arch/x86_64/inspect_json.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
int main(void) {
    const char* valid="{\"id\":7,\"result\":{\"a\":[true,false,null,-12.25e-2,\"hello\\u0041\"]}}";
    assert(json_valid(valid));char text[512];size_t length=strlen(valid);
    for(size_t n=0;n<length;n++){memcpy(text,valid,n);text[n]=0;assert(!json_valid(text));}
    const char* malformed[]={"[01]","[-]","[.1]","[1.]","[1e+]","[NaN]","[nullx]","[truefalse]","{\"x\" 1}","{1:2}","[1,,2]","[1,]","\"\\u012G\"","\"\\u12\"","\"\\x01\"","{}x"};
    for(size_t i=0;i<sizeof(malformed)/sizeof(*malformed);i++)assert(!json_valid(malformed[i]));
    const char* object="{\"nested\":{\"id\":99},\"text\":\"\\\"sessionId\\\":\\\"wrong\\\"\",\"id\" : 7,\"sessionId\":\"real-ID_12\"}";
    assert(json_valid(object));struct json_span span={object,object+strlen(object)};char id[32];
    assert(json_id(json_member(span,"id"),7)&&!json_id(json_member(span,"id"),99));
    assert(json_identifier(json_member(span,"sessionId"),id,sizeof(id))&&!strcmp(id,"real-ID_12"));
    assert(!json_identifier(json_member(span,"sessionId"),id,5)&&!json_member(span,"missing").begin);
    assert(json_equal(json_member(span,"sessionId"),"real-ID_12")&&!json_equal(json_member(span,"text"),"wrong"));
    const char* unsafe="\"a\\\"b\"";assert(!json_identifier((struct json_span){unsafe,unsafe+strlen(unsafe)},id,sizeof(id)));
    const char* overflow="4294967296";assert(!json_id((struct json_span){overflow,overflow+strlen(overflow)},0));
    for(unsigned n=64;n<=65;n++){memset(text,'[',n);text[n]='0';memset(text+n+1,']',n);text[2*n+1]=0;assert(json_valid(text)==(n==64));}
    puts("PASS native DevTools JSON: truncation, grammar, nested fields, escaped text, IDs and bounded nesting");
}
