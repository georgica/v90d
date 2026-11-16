/* by hand / from the namespace test: nat_try add A.B.C.D | del */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include "nat.h"
int main(int argc, char **argv)
{
    int r;
    if (argc >= 3 && !strcmp(argv[1], "add")) r = nat_masquerade(ntohl(inet_addr(argv[2])));
    else if (argc >= 2 && !strcmp(argv[1], "del")) r = nat_remove();
    else return 2;
    if (r) { printf("failed: %s\n", strerror(errno)); return 1; }
    printf("ok\n");
    return 0;
}
