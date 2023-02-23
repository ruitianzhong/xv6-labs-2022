#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char const *argv[])
{
    if (argc != 2)
    {
        fprintf(2, "Usage:sleep time\n");
        exit(1);
    }
    if (sleep(atoi(argv[1])) < 0)
    {
        fprintf(2, "sleep failed\n");
    }
    exit(0);
}
