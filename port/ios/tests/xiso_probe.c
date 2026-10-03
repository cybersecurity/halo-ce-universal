#include "xiso.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t cancel_after, previous;
static int progress(void *context, const char *file, uint64_t done, uint64_t total) {
    (void)context;(void)file;
    if (done<previous || done>total) abort();
    previous=done;
    return !cancel_after || done<cancel_after;
}
int main(int argc, char **argv) {
    char error[1024]={0};int ok;
    if (argc<3) return 2;
    if (!strcmp(argv[1],"ready")) ok=xiso_maps_ready(argv[2],error,sizeof(error));
    else {
        if (argc<4) return 2;
        if (argc>4) cancel_after=strtoull(argv[4],NULL,10);
        ok=xiso_extract_maps(argv[2],argv[3],progress,NULL,error,sizeof(error));
    }
    printf("%s: %s\n",ok?"OK":"ERROR",ok?"maps validated":error);
    return ok?0:1;
}
