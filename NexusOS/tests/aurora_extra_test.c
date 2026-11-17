/* Runs in the native userspace; each injected failure must preserve the old note. */
#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
static int fail_write,fail_sync,fail_rename;
static int failed_writes,failed_syncs,failed_renames;
static jmp_buf dispatch_return;
static const char *dispatch_program,*dispatch_argument;
static pid_t test_fork(void){return 0;}
static int test_execl(const char *program,const char *name,...){
    assert(!strcmp(program,dispatch_program)&&!strcmp(program,name));va_list args;va_start(args,name);
    assert(!strcmp(va_arg(args,const char *),dispatch_argument));assert(va_arg(args,const char *)==NULL);va_end(args);longjmp(dispatch_return,1);
}
void aurora_child_disconnect(void){} /* This fixture has no display connection. */
static ssize_t test_write(int fd,const void *data,size_t size){if(fail_write){failed_writes++;errno=ENOSPC;return -1;}return write(fd,data,size);}
static int test_fsync(int fd){if(fail_sync){failed_syncs++;errno=EIO;return -1;}return fsync(fd);}
static int test_rename(const char *from,const char *to){if(fail_rename){failed_renames++;errno=EIO;return -1;}return rename(from,to);}
#define write test_write
#define fsync test_fsync
#define rename test_rename
#define fork test_fork
#define execl test_execl
#include "../arch/x86_64/aurora_extra.c"
#undef write
#undef fsync
#undef rename
#undef fork
#undef execl

static void expect_file(const char *expected){
    int fd=open(path,O_RDONLY);assert(fd>=0);char bytes[4096];ssize_t n=read(fd,bytes,sizeof(bytes));assert(n==(ssize_t)strlen(expected));assert(!memcmp(bytes,expected,(size_t)n));assert(!close(fd));
    char folder[PATH_MAX];strcpy(folder,path);*strrchr(folder,'/')=0;DIR *dir=opendir(folder);assert(dir);struct dirent *entry;while((entry=readdir(dir)))assert(!strstr(entry->d_name,".aurora-"));assert(!closedir(dir));
}
static void edit(const char *data){assert(reserve(strlen(data)+1));strcpy(text,data);length=strlen(data);dirty=true;}
static void save_success(void){bool success=notes_save();if(!success)fprintf(stderr,"AURORA SAVE FAILED errno=%d message=%s\n",errno,message);assert(success);}
static void package_dispatch(const char *folder,const char *name){
    strcpy(path,folder);char package[PATH_MAX];assert(snprintf(package,sizeof(package),"%s/%s",folder,name)<(int)sizeof(package));
    int fd=open(package,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0&&write(fd,"!<arch>\n",8)==8&&!close(fd));
    files_refresh();assert(entry_count==1);selected=0;char enumerated[PATH_MAX];assert(snprintf(enumerated,sizeof(enumerated),"%s/%s",path,entries[0])<(int)sizeof(enumerated));
    dispatch_program="/bin/xdg-open";dispatch_argument=enumerated;
    if(!setjmp(dispatch_return)){files_open();assert(!"Package was not dispatched to the installer");}
    free_entries();assert(!unlink(package));
}
int main(void){
    char line[128],*args[8];strcpy(line,"echo 'hello world' \"two words\" escaped\\ value ''");assert(arguments(line,args,8)==5);assert(!strcmp(args[0],"echo"));assert(!strcmp(args[1],"hello world"));assert(!strcmp(args[2],"two words"));assert(!strcmp(args[3],"escaped value"));assert(!strcmp(args[4],""));assert(!args[5]);
    strcpy(line,"echo 'open");assert(arguments(line,args,8)==-1&&errno==EINVAL);
    strcpy(line,"echo broken\\");assert(arguments(line,args,8)==-1&&errno==EINVAL);
    strcpy(line,"a b c");assert(arguments(line,args,3)==-1&&errno==E2BIG);
    strcpy(line," \t ");assert(!arguments(line,args,8)&&!args[0]);
    char folder[]="/tmp/nexus-aurora-test-XXXXXX";assert(mkdtemp(folder));assert(snprintf(path,sizeof(path),"%s/notes.txt",folder)<(int)sizeof(path));
    assert(reserve(1));text[0]=0;notes_load();assert(!load_failed&&!existed&&!length);
    edit("original saved note\nsecond line\n");save_success();assert(!dirty);expect_file("original saved note\nsecond line\n");
    edit("replacement");fail_write=1;assert(!notes_save()&&dirty&&failed_writes==1);fail_write=0;expect_file("original saved note\nsecond line\n");
    fail_sync=1;assert(!notes_save()&&dirty&&failed_syncs==1);fail_sync=0;expect_file("original saved note\nsecond line\n");
    fail_rename=1;assert(!notes_save()&&dirty&&failed_renames==1);fail_rename=0;expect_file("original saved note\nsecond line\n");
    save_success();expect_file("replacement");
    edit("short");save_success();expect_file("short");
    length=cursor=0;notes_load();assert(!load_failed&&length==5&&!memcmp(text,"short",5));
    int fd=open(path,O_WRONLY|O_TRUNC);assert(fd>=0&&write(fd,"external",8)==8&&!close(fd));edit("must not overwrite");assert(!notes_save());expect_file("external");
    assert(!unlink(path));package_dispatch(folder,"HELLO.DEB");package_dispatch(folder,"hello_mixed.DeB");package_dispatch(folder,"hello_long.deb");
    assert(!rmdir(folder));free(text);text=NULL;puts("[64] AURORA BACKEND TEST PASSED: parser, save, reload, truncate, ENOSPC, sync, rename, external change, package case dispatch");return 0;
}
