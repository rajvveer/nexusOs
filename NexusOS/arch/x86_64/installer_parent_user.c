/* The desktop must outlive a browser that leaves its installer running. */
#include <stdio.h>
#include <unistd.h>
int main(int argc,char** argv){
    if(argc!=2)return 2;
    pid_t child=fork();if(child<0)return 3;
    if(!child){usleep(100000);execl("/bin/xdg-open","xdg-open",argv[1],(char*)NULL);_exit(4);}
    puts("[64] INSTALLER LAUNCHER EXIT");return 0;
}
