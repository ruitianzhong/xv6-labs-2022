#include <user/user.h>
#include <kernel/fs.h>
#include <kernel/fcntl.h>
#include <kernel/stat.h>
#include <kernel/types.h>

void find(char *, char *);
int main(int argc, char *argv[])
{
    int fd;
    if (argc != 3)
    {
        fprintf(2, "usage: find <directory name> <pattern>\n");
        exit(0);
    }
    struct stat st;
    if ((fd = open(argv[1], O_RDONLY)) < 0)
    {
        fprintf(2, "find : No such directory\n");
        exit(0);
    }
    if (fstat(fd, &st) < 0)
    {
        close(fd);
        fprintf(2, "find: can not stat %s\n", argv[1]);
        exit(0);
    }
    if (T_DIR == st.type)
    {
        close(fd);
        find(argv[1], argv[2]);
    }
    else
    {
        fprintf(2, "usage find <directory name> <pattern>\n");
        close(fd);
    }

    exit(0);
}

void find(char *path, char *pattern)
{
    int fd;
    char buf[512], *p;
    struct dirent d;
    struct stat st;
    if ((fd = open(path, O_RDONLY)) < 0) // error here
    {
        fprintf(2, "find: '%s': No such file or directory\n", path);
        return;
    }

    if (fstat(fd, &st) < 0)
    {
        fprintf(2, "find: can not stat %s \n", path);
        close(fd); // avoid resource leak,highly important
        return;
    }
    switch (st.type)
    {
    case T_DIR:
        if (strlen(path) + 1 + DIRSIZ + 1 > sizeof buf)
        {
            fprintf(2, "find: '%s': Path too long\n", path);
            break; // break is better
        }

        strcpy(buf, path);
        p = buf + strlen(buf); // forget to initialize the pointer firstly
        *p++ = '/';
        while (read(fd, &d, sizeof(d)) == sizeof(d))
        {

            if (0 == d.inum || !strcmp(d.name, ".") || !strcmp(d.name, ".."))
            {
                continue;
            }

            memcpy(p, d.name, DIRSIZ);
            p[DIRSIZ] = 0;
            if (strcmp(d.name, pattern) == 0)
            {
                printf("%s\n", buf);
            }

            find(buf, pattern);
        }
        break;
    default:
      break;
    }
    close(fd); // open and close
    return;
}