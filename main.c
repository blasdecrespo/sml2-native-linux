/* Native Linux host. Rendering/input/time only; game behavior lives in C modules. */
#include "game.h"
#include "audio.h"
#include "savefile.h"
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    Display *display;
    Window window;
    GC gc;
    XImage *image;
    Atom close_message;
    unsigned long palette[4];
    unsigned scale;
    uint8_t held;
} Host;

typedef struct { unsigned frame; uint8_t buttons; } InputEvent;
typedef struct { InputEvent *events; size_t count; } InputSequence;

static uint8_t host_divider(void *context) {
    (void)context;
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now)) return 0;
    /* FF04 increments at 16384Hz. Whole seconds wrap its8-bit value exactly.
     * The native timer supplies entropy; original instruction-phase alignment
     * is not implied by this clock adaptation. */
    return (uint8_t)((uint64_t)now.tv_nsec*16384/1000000000);
}

static uint8_t host_timer(void *context) {
    (void)context;
    /* Retained original gameplay/world captures have TIMA=0,TMA=0,TAC=F8:
     * the programmable timer is disabled. DIV remains free-running. */
    return 0;
}

static bool csv_number(char **cursor,unsigned maximum,unsigned *out) {
    char *s=*cursor;unsigned value=0;
    if(*s<'0'||*s>'9')return false;
    do {
        unsigned digit=(unsigned)(*s-'0');
        if(value>maximum/10||(value==maximum/10&&digit>maximum%10))return false;
        value=value*10+digit;s++;
    } while(*s>='0'&&*s<='9');
    *cursor=s;*out=value;return true;
}

static bool read_events(const char *path,InputSequence *sequence) {
    FILE *file=fopen(path,"r");if(!file){perror(path);return false;}
    char line[64];size_t capacity=0,line_number=0;bool ok=true;
    while(fgets(line,sizeof(line),file)) {
        ++line_number;
        char *cursor=line;unsigned frame,buttons;
        if(!csv_number(&cursor,UINT_MAX,&frame)||*cursor++!=','||
           !csv_number(&cursor,255,&buttons)) {ok=false;break;}
        if(*cursor=='\r')cursor++;
        if(*cursor=='\n')cursor++;
        if(*cursor||(sequence->count&&frame<=sequence->events[sequence->count-1].frame)) {ok=false;break;}
        if(sequence->count==capacity) {
            size_t next=capacity?capacity*2:64;
            if(next<capacity||next>SIZE_MAX/sizeof(InputEvent)) {ok=false;break;}
            InputEvent *grown=realloc(sequence->events,next*sizeof(InputEvent));
            if(!grown){ok=false;break;}
            sequence->events=grown;capacity=next;
        }
        sequence->events[sequence->count++]=(InputEvent){frame,(uint8_t)buttons};
    }
    if(ferror(file))ok=false;
    if(fclose(file))ok=false;
    if(!ok){fprintf(stderr,"Invalid events CSV at line %zu: %s\n",line_number,path);free(sequence->events);*sequence=(InputSequence){0};}
    return ok;
}

static uint8_t key_button(KeySym key) {
    switch(key) {
        case XK_Right: return BUTTON_RIGHT;
        case XK_Left: return BUTTON_LEFT;
        case XK_Up: return BUTTON_UP;
        case XK_Down: return BUTTON_DOWN;
        case XK_z: case XK_Z: return BUTTON_A;
        case XK_x: case XK_X: return BUTTON_B;
        case XK_BackSpace: case XK_Shift_R: return BUTTON_SELECT;
        case XK_Return: return BUTTON_START;
        default: return 0;
    }
}

static void host_close(Host *h) {
    if(h->image) XDestroyImage(h->image);
    if(h->display) {
        if(h->gc) XFreeGC(h->display,h->gc);
        if(h->window) XDestroyWindow(h->display,h->window);
        XCloseDisplay(h->display);
    }
    memset(h,0,sizeof(*h));
}

static bool host_open(Host *h,unsigned scale) {
    h->scale=scale;
    h->display=XOpenDisplay(NULL);
    if(!h->display) { fputs("Cannot open X11 display. Use --headless for verification.\n",stderr); return false; }
    int screen=DefaultScreen(h->display);
    unsigned w=GAME_WIDTH*scale, height=GAME_HEIGHT*scale;
    h->window=XCreateSimpleWindow(h->display,RootWindow(h->display,screen),0,0,w,height,0,0,0);
    h->gc=XCreateGC(h->display,h->window,0,NULL);
    XStoreName(h->display,h->window,"Super Mario Land 2 — native C port (in development)");
    XSelectInput(h->display,h->window,KeyPressMask|KeyReleaseMask|FocusChangeMask|ExposureMask);
    Bool supported=False;
    XkbSetDetectableAutoRepeat(h->display,True,&supported);
    h->close_message=XInternAtom(h->display,"WM_DELETE_WINDOW",False);
    XSetWMProtocols(h->display,h->window,&h->close_message,1);
    XSizeHints hints={0};
    hints.flags=PMinSize|PMaxSize;
    hints.min_width=hints.max_width=(int)w;
    hints.min_height=hints.max_height=(int)height;
    XSetWMNormalHints(h->display,h->window,&hints);
    h->image=XCreateImage(h->display,DefaultVisual(h->display,screen),
        (unsigned)DefaultDepth(h->display,screen),ZPixmap,0,NULL,w,height,32,0);
    if(!h->image) { host_close(h); return false; }
    h->image->data=calloc(height,(size_t)h->image->bytes_per_line);
    if(!h->image->data) { host_close(h); return false; }
    for(unsigned i=0;i<4;i++) {
        XColor c={0}; c.red=c.green=c.blue=(unsigned short)((3-i)*21845);
        if(!XAllocColor(h->display,DefaultColormap(h->display,screen),&c)) {
            host_close(h); return false;
        }
        h->palette[i]=c.pixel;
    }
    XMapWindow(h->display,h->window);
    return true;
}

static bool host_events(Host *h) {
    while(XPending(h->display)) {
        XEvent e; XNextEvent(h->display,&e);
        if(e.type==ClientMessage&&(Atom)e.xclient.data.l[0]==h->close_message) return false;
        if(e.type==FocusOut) h->held=0;
        if(e.type==KeyPress||e.type==KeyRelease) {
            if(e.type==KeyRelease&&XEventsQueued(h->display,QueuedAfterReading)) {
                XEvent next; XPeekEvent(h->display,&next);
                if(next.type==KeyPress&&next.xkey.time==e.xkey.time&&next.xkey.keycode==e.xkey.keycode) continue;
            }
            KeySym k=XLookupKeysym(&e.xkey,0);
            if(e.type==KeyPress&&k==XK_Escape) return false;
            uint8_t button=key_button(k);
            if(e.type==KeyPress) h->held|=button;
            else h->held&=(uint8_t)~button;
        }
    }
    return true;
}

static void host_present(Host *h,const uint8_t *shades) {
    for(unsigned y=0;y<GAME_HEIGHT;y++) for(unsigned x=0;x<GAME_WIDTH;x++)
        for(unsigned dy=0;dy<h->scale;dy++) for(unsigned dx=0;dx<h->scale;dx++)
            XPutPixel(h->image,(int)(x*h->scale+dx),(int)(y*h->scale+dy),h->palette[shades[y*GAME_WIDTH+x]&3]);
    XPutImage(h->display,h->window,h->gc,h->image,0,0,0,0,GAME_WIDTH*h->scale,GAME_HEIGHT*h->scale);
    XFlush(h->display);
}

static bool unsigned_arg(const char *s,unsigned *out) {
    char *end; errno=0; unsigned long n=strtoul(s,&end,10);
    if(errno||!*s||*end||*s=='-'||n>UINT_MAX) return false;
    *out=(unsigned)n; return true;
}

static bool save_capture(const char *directory,const uint8_t *shades,const Game *game) {
    char path[4096];
    if(snprintf(path,sizeof(path),"%s/screen.bin",directory)>=(int)sizeof(path)) return false;
    FILE *f=fopen(path,"wb"); if(!f) { perror(path); return false; }
    bool ok=fwrite(shades,1,GAME_WIDTH*GAME_HEIGHT,f)==GAME_WIDTH*GAME_HEIGHT;
    if(fclose(f)) ok=false;
    if(!ok) return false;
    snprintf(path,sizeof(path),"%s/screen.pgm",directory);
    f=fopen(path,"wb"); if(!f) return false;
    fprintf(f,"P5\n160 144\n255\n");
    for(unsigned i=0;i<GAME_WIDTH*GAME_HEIGHT;i++) fputc(255-85*shades[i],f);
    ok=!ferror(f); if(fclose(f)) ok=false; if(!ok)return false;
    snprintf(path,sizeof(path),"%s/state.json",directory);
    f=fopen(path,"w");if(!f)return false;
    game_write_state(game,f);
    ok=!ferror(f);if(fclose(f))ok=false;return ok;
}

static void usage(const char *program) {
    fprintf(stderr,"Usage: %s [--headless] [--frames N] [--events CSV] [--output DIRECTORY] [--start-level] [--scale 1..8] [--mute] [--save PATH]\n"
        "Controls: arrows move, Z jump/A, X run/B, Enter start, Backspace select, Escape quit.\n"
        "Save: original 8192-byte format; defaults to ./sml2.sav in windowed mode. Headless saves require --save.\n"
        "Events CSV: frame,held_bitmask; bits right=1,left=2,up=4,down=8,A=16,B=32,select=64,start=128.\n",program);
}

int main(int argc,char **argv) {
    bool headless=false,start_level=false,has_limit=false,mute=false;
    unsigned frames=0,scale=3;const char *event_path=NULL,*output=NULL,*save_path=NULL;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--headless")) headless=true;
        else if(!strcmp(argv[i],"--mute")) mute=true;
        else if(!strcmp(argv[i],"--start-level")) start_level=true;
        else if(!strcmp(argv[i],"--help")) { usage(argv[0]); return 0; }
        else if(!strcmp(argv[i],"--frames")&&i+1<argc) { has_limit=unsigned_arg(argv[++i],&frames); if(!has_limit){usage(argv[0]);return 2;} }
        else if(!strcmp(argv[i],"--scale")&&i+1<argc) { if(!unsigned_arg(argv[++i],&scale)||scale<1||scale>8){usage(argv[0]);return 2;} }
        else if(!strcmp(argv[i],"--events")&&i+1<argc) event_path=argv[++i];
        else if(!strcmp(argv[i],"--output")&&i+1<argc) output=argv[++i];
        else if(!strcmp(argv[i],"--save")&&i+1<argc) save_path=argv[++i];
        else { usage(argv[0]); return 2; }
    }
    if(headless&&!has_limit) { fputs("--headless requires --frames.\n",stderr); return 2; }
    InputSequence sequence={0};size_t event_index=0;
    if(event_path&&!read_events(event_path,&sequence))return 2;
    Game *game=game_create(start_level);if(!game){free(sequence.events);return 1;}
    game_set_timer_readers(game,host_divider,host_timer,NULL);
    if(!headless&&!save_path) save_path="sml2.sav";
    SaveFile save={0};
    if(save_path&&(!savefile_load(&save,save_path)||
       (save.exists&&!game_import_save(game,save.bytes,sizeof(save.bytes))))) {
        perror(save_path);game_destroy(game);free(sequence.events);return 1;
    }
    Host host={0};
    if(!headless&&!host_open(&host,scale)) {game_destroy(game);free(sequence.events);return 1;}
    uint64_t saved_revision=game_save_revision(game);
    if(save_path) {
        uint8_t bytes[GAME_SAVE_SIZE];game_export_save(game,bytes);
        if(!savefile_store(&save,bytes)) {
            perror(save_path);host_close(&host);game_destroy(game);free(sequence.events);return 1;
        }
    }
    bool sound=!headless&&!mute&&audio_open();
    if(sound){audio_reset();game_set_audio_output(game,audio_write_register,NULL);}
    uint8_t shades[GAME_WIDTH*GAME_HEIGHT],held=0;unsigned completed=0;
    struct timespec deadline;clock_gettime(CLOCK_MONOTONIC,&deadline);
    int status=0;
    while(!has_limit||completed<frames) {
        if(!headless) { if(!host_events(&host))break; if(!event_path)held=host.held; }
        if(event_index<sequence.count&&sequence.events[event_index].frame==completed)
            held=sequence.events[event_index++].buttons;
        game_step(game,held);game_render(game,shades);completed++;
        if(save_path&&saved_revision!=game_save_revision(game)) {
            uint8_t bytes[GAME_SAVE_SIZE];game_export_save(game,bytes);
            if(!savefile_store(&save,bytes)) {perror(save_path);status=1;break;}
            saved_revision=game_save_revision(game);
        }
        if(!headless) {
            if(sound) {
                audio_frame();
            }
            host_present(&host,shades);
            /* Original DMG frame rate: 4,194,304 clocks / 70,224 clocks per frame. */
            deadline.tv_nsec+=16742706;
            if(deadline.tv_nsec>=1000000000L){deadline.tv_sec++;deadline.tv_nsec-=1000000000L;}
            while(clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&deadline,NULL)==EINTR) {}
        }
    }
    game_render(game,shades);
    if(!status&&output&&!save_capture(output,shades,game)) {perror("capture");status=1;}
    if(headless&&!status) game_write_state(game,stdout);
    if(sound)audio_close();
    host_close(&host);game_destroy(game);free(sequence.events);
    return status;
}
