#include <user/user.h>
#include <kernel/param.h>
int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(2, "usage : xargs <command> [arguments] ...\n");
        exit(0);
    }
    char *args[MAXARG];
    int i;
    for (i = 0; i < argc; i++)
    {
        args[i] = argv[i];
    }
    if (argc + 1 > MAXARG)
    {
        fprintf(2, "xargs: arguments are more than %d\n", MAXARG);
        exit(0);
    }
    char buf[512], byte;
    i = 0;
    while (read(0, &byte, 1) == 1)
    {
        if (i >= sizeof buf)
        {
            fprintf(2, "xargs: argument too long\n");
            exit(0);
        }
        if ('\n' == byte)
        {
            buf[++i] = 0;
            i = 0;
            args[argc] = buf;
            if (fork() == 0)
            {
                exec(args[1], args + 1);
            }
            wait(0);
        }
        else
        {
            buf[i++] = byte;
        }
    }
    exit(0);
}