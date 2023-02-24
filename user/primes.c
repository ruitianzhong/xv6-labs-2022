#include <user/user.h>
void compute_routine(int);
int main(int argc, char const *argv[])
{
    int i = 2;
    int fd[2];
    if (pipe(fd) < 0)
    {
        fprintf(2, "primes: pipe fails\n");
        exit(0);
    }
    if (fork() == 0)
    {
        close(fd[1]);
        compute_routine(fd[0]);
    }
    else
    {
        close(fd[0]);
        for (i = 2; i <= 35; i++)
        {
            write(fd[1], &i, sizeof(int));
        }
        close(fd[1]);
        wait(0);
    }
    exit(0);
}

void compute_routine(int fd)
{

    int i, temp;
    int s = fd;
loop:
    read(s, &i, sizeof(int));
    printf("prime %d\n", i);
    int flag = 0;
    int fds[2];
    while (read(s, &temp, sizeof(int)) == 4)
    {
        
        if (temp % i != 0)
        {
            if (temp == 9 && i == 3)
            {
                printf("Note::\n");
            }
            if (0 == flag)
            {
                pipe(fds);
                if (fork() == 0)
                {
                    close(fds[1]);
                    s = fds[0];
                    goto loop;
                }
                else
                {
                    close(fds[0]);
                    write(fds[1], &temp, sizeof(int));
                    flag = 1;
                }
            }
            else
            {
                write(fds[1], &temp, sizeof(int));
            }
        }
        }
    close(fd);
    close(fds[1]);
    wait(0);
    exit(0);
}