#include "savefile.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool savefile_load(SaveFile *save,const char *path) {
    *save=(SaveFile){.path=path};
    FILE *file=fopen(path,"rb");
    if(!file) {
        if(errno!=ENOENT) return false;
        save->backed_up=true; /* No original file exists to preserve. */
        return true;
    }
    struct stat st;
    bool ok=fstat(fileno(file),&st)==0;
    if(ok&&(!S_ISREG(st.st_mode)||st.st_size!=GAME_SAVE_SIZE)) {errno=EINVAL;ok=false;}
    if(ok&&fread(save->bytes,1,GAME_SAVE_SIZE,file)!=GAME_SAVE_SIZE) {
        if(!ferror(file)) errno=EINVAL;
        ok=false;
    }
    int error=errno;
    if(fclose(file)&&ok) {ok=false;error=errno;}
    if(!ok) {errno=error;return false;}
    save->exists=true;
    return true;
}

static bool write_bytes(int fd,const uint8_t *bytes,size_t size) {
    while(size) {
        ssize_t count=write(fd,bytes,size);
        if(count<0&&errno==EINTR) continue;
        if(count<=0) {if(!count)errno=EIO;return false;}
        bytes+=count;size-=(size_t)count;
    }
    return fsync(fd)==0;
}

static bool backup_original(SaveFile *save,char *path) {
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd<0) return errno==EEXIST;
    bool ok=write_bytes(fd,save->bytes,GAME_SAVE_SIZE);
    int error=errno;
    if(close(fd)&&ok) {ok=false;error=errno;}
    if(!ok) {unlink(path);errno=error;}
    return ok;
}

bool savefile_store(SaveFile *save,const uint8_t bytes[GAME_SAVE_SIZE]) {
    if(save->exists&&!memcmp(save->bytes,bytes,GAME_SAVE_SIZE)) return true;
    size_t length=strlen(save->path);
    if(length>SIZE_MAX-12) {errno=ENAMETOOLONG;return false;}
    char *path=malloc(length+12);
    if(!path) return false;
    memcpy(path,save->path,length);
    if(save->exists&&!save->backed_up) {
        memcpy(path+length,".bak",5);
        if(!backup_original(save,path)) {free(path);return false;}
        save->backed_up=true;
    }
    memcpy(path+length,".tmp.XXXXXX",12);
    int fd=mkstemp(path);
    if(fd<0) {free(path);return false;}
    bool ok=write_bytes(fd,bytes,GAME_SAVE_SIZE);
    int error=errno;
    if(close(fd)&&ok) {ok=false;error=errno;}
    if(ok&&rename(path,save->path)) {ok=false;error=errno;}
    if(!ok) unlink(path);
    free(path);
    if(!ok) {errno=error;return false;}
    memcpy(save->bytes,bytes,GAME_SAVE_SIZE);
    save->exists=true;
    return true;
}
