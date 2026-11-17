#include <assert.h>
#include <stdio.h>
#include "../arch/x86_64/local_file.h"
int main(void){char out[256];
    assert(local_file("/home/nexus/a%20b.deb",NULL,out,sizeof(out))&&!strcmp(out,"/home/nexus/a%20b.deb"));
    assert(local_file("file:///home/nexus/a%20b.deb",NULL,out,sizeof(out))&&!strcmp(out,"/home/nexus/a b.deb"));
    assert(local_file("FILE://localhost/home/nexus/a%23b.deb",NULL,out,sizeof(out))&&!strcmp(out,"/home/nexus/a#b.deb"));
    assert(local_file("hello.deb","/home/nexus/Downloads",out,sizeof(out))&&!strcmp(out,"/home/nexus/Downloads/hello.deb"));
    const char* bad[]={"","https://example.com/a.deb","file://remote/abc","file:///a%00b","file:///a%2","file:///a%GG","file:///a?query","file:///a#fragment","file:///a%0Ab","/a\\b"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)assert(!local_file(bad[i],"/",out,sizeof(out)));
    assert(!local_file("/abcdef",NULL,out,4));assert(!local_file("relative",NULL,out,sizeof(out)));
    puts("PASS local package paths: literal names, local URI decoding, remote/control/truncation rejection");
}
