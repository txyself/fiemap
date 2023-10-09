#include <stdlib.h>
#include "filegrag.h"


int main(){

    char filename[] = "/home/xgc/file/hello.txt";
    system("sync");
    unsigned long long filesize;
    extent_List *extents = (extent_List*)malloc(sizeof(extent_List));
    get_extent_info(filename, extents, &filesize);
    printf("%d extent found\n", extents->size);

    extent_Node* p = extents->headnode.next;
    int i = 0;
    while(p){
        printf("extent %d:\n", i);
        printf("fe_flags = %d\n", p->extent.fe_flags);
        printf("fe_length = %llu\n", p->extent.fe_length/4096);
        printf("fe_logical = %llu\n", p->extent.fe_logical/4096);
        printf("fe_physical = %llu\n", p->extent.fe_physical/4096);
        i++;
        p = p->next;
    }

    return 0;
}