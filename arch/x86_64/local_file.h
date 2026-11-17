#ifndef NEXUS_LOCAL_FILE_H
#define NEXUS_LOCAL_FILE_H
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
static int local_hex(unsigned char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
/* Decode local file URIs only; leave ordinary filenames containing '%' unchanged. */
static bool local_file(const char* input,const char* cwd,char* output,size_t capacity){
    if(!input||!*input||capacity<2)return false;
    bool uri=strlen(input)>=7;
    const char prefix[]="file://";
    for(unsigned i=0;uri&&i<7;i++){unsigned char c=input[i];if(c>='A'&&c<='Z')c+=32;if(c!=(unsigned char)prefix[i])uri=false;}
    const char* path=input;
    if(uri){path+=7;if(*path!='/'){if(strncmp(path,"localhost/",10))return false;path+=9;}}
    else if(strstr(path,"://")||!strncmp(path,"file:",5))return false;
    size_t used=0;
    if(*path!='/'){
        if(!cwd||cwd[0]!='/'||strlen(cwd)+2>=capacity)return false;
        used=strlen(cwd);memcpy(output,cwd,used);if(output[used-1]!='/')output[used++]='/';
    }
    while(*path){unsigned char c=*path++;
        if(uri&&(c=='?'||c=='#'))return false;
        if(uri&&c=='%'){if(!path[0]||!path[1])return false;int a=local_hex(path[0]),b=local_hex(path[1]);if(a<0||b<0)return false;c=(unsigned char)((a<<4)|b);path+=2;}
        if(c<32||c==127||c=='\\'||c==':'||used+1>=capacity)return false;
        output[used++]=(char)c;
    }
    output[used]=0;return used>0;
}
#define NEXUS_PACKAGE_RECEIPT ".nexus-package-sha256"
#endif
