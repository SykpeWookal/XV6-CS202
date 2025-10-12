#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"


int main(int argc, char *argv[]){
    int n = 5; // Default value
    if(argc>=2)
        n = atoi(argv[1]);
    hello(n);
    exit(0);
}