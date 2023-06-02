#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include "./header/Sever.h"
int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        printf("./a.out port path\n");
        return -1;
    }
    unsigned short port = atoi(argv[1]);
    // 切换服务器的工作路径
    chdir(argv[2]);
    int lfd = initListenFd(port);
    // 启动服务器程序
    epollRun(lfd);
    return 0;
}