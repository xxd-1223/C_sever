#include "../header/Sever.h"
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <assert.h>
#include <sys/sendfile.h>
#include <dirent.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
struct FdInfo
{
    int fd;
    int epfd;
    pthread_t tid;
};

int initListenFd(unsigned short port)
{
    // 1，创建监听的fd
    int lfd = socket(AF_INET, SOCK_STREAM, 0); // AF_INET指定ipv协议，SOCK_STREAM指定基于流式协议还是报文协议，0表示使用的是流式协议中的TCP协议
    if (lfd == -1)
    {
        perror("socket");
        return -1;
    }
    // 2，设置端口复用
    int opt = 1;                                                           // 置为1表示lfd绑定的端口可以复用
    int ret = setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt); // 设置套接字的属性
    if (ret == -1)
    {
        perror("setsockopt");
        return -1;
    }
    // 3，绑定
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;         // 指定地址族协议是ipv4还是ipv6
    addr.sin_port = htons(port);       // 指定端口的网络字节序（大端）
    addr.sin_addr.s_addr = INADDR_ANY; // 点分十进制 0地址
    ret = bind(lfd, (struct sockaddr *)&addr, sizeof addr);
    if (ret == -1)
    {
        perror("bind");
        return -1;
    }
    // 4，设置监听
    ret = listen(lfd, 128); // 指定监听的过程中一次性最多能够接收多少连接
    if (ret == -1)
    {
        perror("listen");
        return -1;
    }
    // 5，返回fd
    return lfd;
}

int epollRun(int lfd)
{
    // 1，创建epoll的实例
    int epfd = epoll_create(1); // 参数已被弃用（无实际意义），只需指定1个大于0的数就行，如果参数为0，那么创建失败并返回-1
    if (epfd == -1)
    {
        perror("epoll create");
        return -1;
    }
    // 2，lfd上树
    struct epoll_event ev;
    ev.data.fd = lfd;    // 记录当前要操作的文件描述符
    ev.events = EPOLLIN; // 指定lfd对应的事件
    /* epoll_ctl的参数
    epoll树的根节点，
    指定epoll_ctl要做的事情（往树上添加节点&删除节点&把已经添加到树上的文件描述符的事件做更改），
    要操作的文件描述符
    结构体，把lfd的相关信息放入
    */
    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
    if (ret == -1)
    {
        perror("epoll_ctl");
        return -1;
    }
    // 3，检测
    struct epoll_event evs[1024];
    int size = sizeof(evs) / sizeof(struct epoll_event);
    while (1)
    {
        /*  epoll_wait 委托内核检测通过epoll_ctl添加到epoll模型上的文件描述符对应的事件是否被激活
        第4个参数 time_out，epoll_wait在阻塞期间没有任何事件被激活，那么在经过time_out后解除阻塞；
        如果为-1，一直被阻塞直到epoll树上某些文件描述符上的事件被激活后才解除阻塞，然后把激活的事件对应的
        消息存储到evs中，返回被触发的事件的个数
        */
        int num = epoll_wait(epfd, evs, size, -1);
        for (int i = 0; i < num; i++)
        {
            struct FdInfo *info = (struct FdInfo *)malloc(sizeof(struct FdInfo));
            int fd = evs[i].data.fd;
            info->epfd = epfd;
            info->fd = fd;
            if (fd == lfd) // 判断返回的文件描述符是用于监听的文件描述符读事件或是用于通信的文件描述符读事件
            {
                // 建立新连接 accept
                // acceptClient(lfd, epfd);
                pthread_create(&info->tid, NULL, acceptClient, info);
            }
            else // 用于通信的文件描述符
            {
                // 主要是接收对端的数据
                // recvHttpRequest(fd, epfd);
                pthread_create(&info->tid, NULL, recvHttpRequest, info);
            }
        }
    }
}

void *acceptClient(void *arg)
{
    // 1，建立连接
    struct FdInfo *info = (struct FdInfo *)arg;
    int cfd = accept(info->fd, NULL, NULL); // 用于监听的文件描述符；（传出参数）保存了客户端的ip和端口信息；第二个参数的大小
    if (cfd == -1)
    {
        perror("accept");
        return NULL;
    }
    // 2，设置为非阻塞
    int flag = fcntl(cfd, F_GETFL); // 得到文件描述符的属性
    flag |= O_NONBLOCK;             // 追加非阻塞的属性
    fcntl(cfd, F_SETFL, flag);      // 设置属性
    // 3，cfd添加到epoll中
    struct epoll_event ev;
    ev.data.fd = cfd;              // 记录当前要操作的文件描述符
    ev.events = EPOLLIN | EPOLLET; // 指定cfd对应的事件，并指定epoll为边沿模式
    /* epoll_ctl的参数
    epoll树的根节点，
    指定epoll_ctl要做的事情（往树上添加节点&删除节点&把已经添加到树上的文件描述符的事件做更改），
    要操作的文件描述符
    结构体，把lfd的相关信息放入
    */
    int ret = epoll_ctl(info->epfd, EPOLL_CTL_ADD, cfd, &ev); // 现在epoll的工作模式为边沿非阻塞
    if (ret == -1)
    {
        perror("epoll_ctl");
        return NULL;
    }
    free(info);
    return 0;
}

void *recvHttpRequest(void *arg)
{
    struct FdInfo *info = (struct FdInfo *)arg;
    int len = 0;
    int total = 0;
    int tmp[1024] = {0};
    char buffer[4096] = {0};
    while ((len = recv(info->fd, tmp, sizeof tmp, 0)) > 0)
    {
        if (total + len < sizeof buffer)
        {
            memcpy(buffer + total, tmp, len);
        }
        total += len;
    }
    // 判断数据是否被接收完毕
    if ((len == -1) && (errno = EAGAIN))
    {
        // 解析请求行
        char *pt = strstr(buffer, "\r\n");
        int reqLen = pt - buffer;
        buffer[reqLen] = '\0';
        parseRequestLine(buffer, info->fd);
    }
    else if (len == 0)
    {
        // 客户端断开了连接，用于通信的文件描述符需从epoll树中删除
        epoll_ctl(info->epfd, EPOLL_CTL_DEL, info->fd, NULL);
        close(info->fd);
    }
    else
    {
        perror("recv");
    }
    free(info);
    return 0;
}

int parseRequestLine(const char *line, int cfd)
{
    // 解析请求行 get /xxx/1.jpg http/1.1
    char method[12];
    char path[1024];
    sscanf(line, "%[^ ] %[^ ]", method, path);
    if (strcasecmp(method, "get") != 0) // 不区分大小写
    {
        return -1;
    }
    // 处理客户端请求的静态资源（目录或文件）
    char *file = NULL;
    if (strcmp(path, "/") == 0)
    {
        file = "./";
    }
    else
    {
        file = path + 1;
    }
    // 获取文件属性
    struct stat st;
    int ret = stat(file, &st);
    if (ret == -1)
    {
        // 文件不存在 回复404
        sendHeadMsg(cfd, 404, "Not Found", getFileType(".html"), -1);
        sendFile("404.html", cfd);
        return 0;
    }
    if (S_ISDIR(st.st_mode))
    {
        // 把目录内容发送给客户端
        sendHeadMsg(cfd, 200, "OK", getFileType(".html"), -1);
        sendDir(file, cfd);
    }
    else
    {
        // 把文件内容发送给客户端
        sendHeadMsg(cfd, 200, "OK", getFileType(file), st.st_size);
        sendFile(file, cfd);
    }

    return 0;
}

int sendFile(const char *fileName, int cfd)
{
    // 1，打开文件
    int fd = open(fileName, O_RDONLY);
    assert(fd > 0);
#if 0
    while (1)
    {
        char buf[1024];
        int len = read(fd, buf, sizeof buf);
        if(len > 0)
        {
            send(cfd, buf, len, 0);
            usleep(10); // 防止发送数据太快
        }
        else if (len == 0)
        {
            break;
        }
        else
        {
            perror("read");
        }
        
    }
#else
    off_t offset = 0;
    int size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    while (offset < size)
    {
        int ret = sendfile(cfd, fd, &offset, size - offset); // 第三个参数 1.发送数据之前根据该偏移量开始读文件数据 2.发送数据之后，更新该偏移量
        if (ret == -1)
        {
            perror("sendfile"); // 由于cfd是非阻塞的
        }
    }
#endif
    close(fd);
    return 0;
}

int sendHeadMsg(int cfd, int status, const char *descr, const char *type, int length)
{
    // 状态行
    char buf[4096] = {0};
    sprintf(buf, "http/1.1 %d %s\r\n", status, descr);
    // 响应头
    sprintf(buf + strlen(buf), "content-type: %s\r\n", type);
    sprintf(buf + strlen(buf), "content-length: %d\r\n\r\n", length);
    send(cfd, buf, strlen(buf), 0);
    return 0;
}

const char *getFileType(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL)
        return "text/plain; charset=utf-8"; // 纯文本
    else if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html; charset=utf-8";
    else if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    return "text/plain; charset=utf-8";
}

int sendDir(const char *dirName, int cfd)
{
    char buf[4096] = {0};
    sprintf(buf, "<html><head><title>%s</title></head><body><table>", dirName);
    struct dirent **namelist;
    int num = scandir(dirName, &namelist, NULL, alphasort);
    for (int i = 0; i < num; i++)
    {
        // 取出文件名或目录名
        char *name = namelist[i]->d_name;
        struct stat st;
        char subPath[1024] = {0};
        sprintf(subPath, "%s/%s", dirName, name); // 拼接字符串
        stat(name, &st);
        if (S_ISDIR(st.st_mode))
        {
            sprintf(buf + strlen(buf), "<tr><td><a href=\"%s/\">%s</a></td><td>%ld</td></tr>", name, name, st.st_size);
        }
        else
        {
            sprintf(buf + strlen(buf), "<tr><td><a href=\"%s\">%s</a></td><td>%ld</td></tr>", name, name, st.st_size);
        }
        send(cfd, buf, strlen(buf), 0);
        memset(buf, 0, sizeof(buf));
        free(namelist[i]);
    }
    sprintf(buf, "</table></body></html>");
    send(cfd, buf, strlen(buf), 0);
    free(namelist);
    return 0;
}