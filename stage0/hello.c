#include <stdio.h>
#include <sys/utsname.h>
int main(void){struct utsname u; uname(&u); printf("hello from v90d on %s %s %s\n", u.sysname, u.release, u.machine); return 0;}
