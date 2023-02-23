#include <user/user.h>

int main(int argc, char const *argv[])
{
    int pipefd1[2];
    int pipefd2[2];
    if (pipe(pipefd1) || pipe(pipefd2))
    {
        exit(0);
    }
    int pid;
    if (fork() != 0)
    {
        char tx = 'a';
        close(pipefd1[0]);
        close(pipefd2[1]);
        write(pipefd1[1], &tx, 1);
        if (read(pipefd2[0], &tx, 1) > 0)
        {
            pid = getpid();
            printf("%d: received pong\n", pid);
        }
        close(pipefd1[1]);
        close(pipefd2[0]);
    }
    else
    {
        char rx;
        close(pipefd1[1]);
        if (read(pipefd1[0], &rx, 1) > 0)
        {
            pid = getpid();
            printf("%d: received ping\n", pid);
            write(pipefd2[1], &rx, 1);
        }
        close(pipefd1[0]);
        close(pipefd2[1]);
    }
    exit(0);
}