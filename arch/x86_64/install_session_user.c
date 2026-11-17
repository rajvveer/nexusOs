#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)){dprintf(2,"[64] INSTALL SESSION FAIL line=%d errno=%d\n",__LINE__,errno);_exit(99);} } while(0)
int main(void) {
    char package[512],destination[512],digest[67],executable[512],argument[32],expected[128];
    FILE* config=fopen("/nexus/install-session","r");CHECK(config);
    char* fields[]={package,destination,digest,executable,argument,expected};size_t sizes[]={sizeof(package),sizeof(destination),sizeof(digest),sizeof(executable),sizeof(argument),sizeof(expected)};
    for(unsigned i=0;i<6;i++){CHECK(fgets(fields[i],sizes[i],config));char* newline=strchr(fields[i],'\n');CHECK(newline);*newline=0;if(newline>fields[i]&&newline[-1]=='\r')newline[-1]=0;}
    CHECK(fgetc(config)==EOF&&!fclose(config));
    CHECK(!mkdir("/home/nexus/Applications",0700)||errno==EEXIST);
    pid_t child=fork();CHECK(child>=0);
    if(!child){execl("/bin/nexus-install","nexus-install",package,destination,digest,(char*)NULL);perror("exec installer");_exit(98);}
    int status;CHECK(waitpid(child,&status,0)==child&&status==0);
    int output[2];CHECK(!pipe(output));child=fork();CHECK(child>=0);
    if(!child){CHECK(dup2(output[1],1)==1);close(output[0]);close(output[1]);execl(executable,executable,*argument?argument:NULL,(char*)NULL);perror("exec installed app");_exit(98);}
    close(output[1]);char text[4096];size_t used=0;
    for(;;){ssize_t n=read(output[0],text+used,sizeof(text)-1-used);if(n<0&&errno==EINTR)continue;CHECK(n>=0);if(!n)break;used+=n;CHECK(used<sizeof(text)-1);}
    text[used]=0;CHECK(!close(output[0])&&waitpid(child,&status,0)==child&&status==0);
    printf("[64] INSTALLED OUTPUT expected=%s output=%s",expected,text);fflush(stdout);
    char* end=strchr(text,'\n');CHECK(end);
    while(end>text&&(end[-1]==' '||end[-1]=='\r'))end--;
    CHECK((size_t)(end-text)==strlen(expected)&&!memcmp(text,expected,(size_t)(end-text)));
    printf("[64] INSTALLED APPLICATION EXECUTED path=%s output=%s",executable,text);puts("[64] INSTALL SESSION PASSED");return 0;
}
