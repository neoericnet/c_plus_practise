//
// Created by yaodi on 2019/11/13.
//

// IO多路复用，事件驱动+非阻塞，实现一个线程完成对多个fd的监控和响应，提升CPU利用率
// epoll优点:
//      1.select需要每次调用select时拷贝fd，epoll_ctl拷贝一次，epoll_wait就不需要重复拷贝
//      2.不需要像select遍历fd做检查，就绪的会被加入就绪list，遍历list完成处理
//      3.没有最大连接限制，与最大文件数目相关：cat /proc/sys/fs/file-max，与内存相关
// epoll实现相关:
//      1.epoll_ctl,将fd的event使用RB tree保存，读写O(logN)；
//      2.一旦有event，内核负责添加到rdlist链表
//      3.epoll_wait检查链表看是否有事件，并进行处理

// Ref
//      https://www.cnblogs.com/lojunren/p/3856290.html
//      http://blog.chinaunix.net/uid-28541347-id-4273856.html


// Question:
//      是否需要每个event一个实例？

#include <cstdlib>              /* exit() */
#include <cstdio>               /* perror(): 打印信息+发生错误的原因，可用于定位。 */
#include <iostream>             /* cin cout */
#include <cstdint>              /* uint32 */
#include <cstring>              /* memset memcpy*/
#include <sys/types.h>          /* 为了满足一些 BSD系统添加头文件*/
#include <sys/socket.h>         /* socket(); listen(); baccept(); socklen_t */
#include <netinet/in.h>         /* struct sockaddr_in:
                                保存socket信息; ntohl(), ntohs(), htonl() and htons()*/
#include <arpa/inet.h>          /* inet_ntoa */
#include <sys/epoll.h>          /* epoll_create(); epoll_ctlstruct epoll_event*/
#include <unistd.h>             /* read() write(), 不是C语言范畴，所以没有cxxxx的实现 */

#include <cerrno>               /* errno */
#include <stdint.h>
// http://minirighi.sourceforge.net/html/errno_8h.html

typedef void (*eventHandleFunc)(void);
typedef struct tzEventHandler{
    eventHandleFunc event_handler_func;
    void *ptr;
    int fd;
}TzEventHandler;

void handlerImpl(void){
  std::cout << "handle an event." << std::endl;
}
void read_handlerImpl(void){
  std::cout << "handle an read event." << std::endl;
}
void send_handlerImpl(void){
  std::cout << "handle an send event." << std::endl;
}

void checEventType(uint32_t type){
  std::cout << "type check:" << std::endl;
  if(type & EPOLLIN) std::cout << "\tEPOLLIN" << std::endl;
  if(type & EPOLLOUT) std::cout << "\tEPOLLOUT" << std::endl;
  if(type & EPOLLRDHUP ) std::cout << "\tEPOLLRDHUP " << std::endl;
  if(type & EPOLLPRI) std::cout << "\tEPOLLPRI" << std::endl;
  if(type & EPOLLERR) std::cout << "\tEPOLLERR" << std::endl;
  if(type & EPOLLHUP) std::cout << "\tEPOLLHUP" << std::endl;
  if(type & EPOLLET) std::cout << "\tEPOLLET" << std::endl;
  if(type & EPOLLONESHOT ) std::cout << "\tEPOLLONESHOT " << std::endl;
  if(type & EPOLLWAKEUP ) std::cout << "\tEPOLLWAKEUP " << std::endl;
}
/**
\brief  错误处理函数
*/
void tzError(const char *msg)
{
  perror(msg);
  exit(1);    // 一般不同原因不同的exit code更为规范
}

int main(int argc, char *argv[]){
  // listen socket
  int listenfd = socket(AF_INET, SOCK_STREAM, 0); // 监听端口非阻塞
  if(listenfd<0){
    tzError("listen socket()");
  }

  // bind
  struct sockaddr_in listen_addr = {0};
  listen_addr.sin_family = AF_INET;
  listen_addr.sin_port = htons(8081);
  listen_addr.sin_addr.s_addr = INADDR_ANY;
  int socket_opt_ret = bind(listenfd, (struct sockaddr*)&listen_addr, sizeof(listen_addr));
  if(socket_opt_ret<0){
    tzError("bind()");
  }

  // listen
#define MAX_LISTEN_TCP 10
  socket_opt_ret = listen(listenfd, MAX_LISTEN_TCP);
  if(socket_opt_ret<0){
    tzError("listen()");
  }

  // epoll
  int epoll_fd = epoll_create(5);
  // int epoll_create(int size);
  // http://man7.org/linux/man-pages/man2/epoll_create.2.html
  // size:    用于告诉kernel可能被添加的caller数量，内核估计需要开辟的空间
  //          2.6.8被忽略，kernel会自动开辟需要的空间。但必须大于0（兼容）。
  // return:  epoll相关句柄，一组连接的管理只需要一个
  if(epoll_fd<0){
    tzError("epoll_create()");
  }

  // 自定义处理对象的设置
  TzEventHandler listen_handler;
  listen_handler.fd = listenfd;   // event相关fd
  listen_handler.event_handler_func = handlerImpl;
  int cnt = 0;
  listen_handler.ptr = &cnt;

  // event设置
  struct epoll_event event = {0}; // Hint:对于结构体，{0}触发聚合初始化，全置0
  // typedef union epoll_data {
  //     void    *ptr;
  //     int      fd;
  //     uint32_t u32;
  //     uint64_t u64;
  // } epoll_data_t;

  // struct epoll_event {
  //     uint32_t     events;    /* Epoll events */
  //     epoll_data_t data;      /* User data variable */
  // };

  // Epoll events:
  //      EPOLLIN     可以read时触发
  // read event:
  //      socket of TCP,三次握手结束，可以accept时
  //      socket 接收缓冲数据>SO_RCVLOWAT，default 1，使用read处理
  //      socket peer关闭连接时，且read为0；如果非阻塞无数据，read返回-1并设置errno=EAGAIN
  //      socket 有未处理的错误，此时可以用getsockopt来读取和清除该错误
  //      EPOLLOUT    可以write时触发
  // write event:
  //      socket 发送缓冲数据>SO_SNDLOWAIT
  //      socket 非阻塞模式下，connect返回之后，发起连接成功或失败
  //      socket上有未处理的错误，此时可以用getsockopt来读取和清除该错误
  //      EPOLLET     边缘触发模式？？？
  //      EPOLLRDHUP  >2.6.17,如果对方close connection或write时对方退出时触发。可用于边缘模式的探测。
  event.events = EPOLLIN;
  event.data.ptr = (void*)&listen_handler;    // 指向处理事件的对象

  // event注册
  epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listenfd, &event);
  // int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
  // http://man7.org/linux/man-pages/man2/epoll_ctl.2.html
  // epfd: epoll实例的fd
  // op:
  //      EPOLL_CTL_ADD   添加事件，将要监听的fd注册，并设置event中的事件进行监听
  //      EPOLL_CTL_MOD   修改事件
  //      EPOLL_CTL_DEL   删除事件，删除对应的df，event参数不为NULL但是被忽略(after 2.6.9可以为NULL)
  // fd:
  //      要被监听事件的fd，所以epoll数量只受文件系统限制
  // event:
  //      指定了监听设置的结构体，包含要监测的事件类型，处理方法
  // 注意：每个fd只能add一次，改变监听事件使用MOD；
  //      如果添加多次fd，视为无效，并errno返回EEXIST，epoll_ctl返回-1

  // event处理
#define BUFSIZE 100     // read接收缓存
#define MAXNFD  10      // 一次最多接受read的数量
  struct epoll_event recv_events[MAXNFD] = {0};   // 用于保存获取的事件队列，依次处理
  int n_ready_event=0;
  char buf[MAXNFD][BUFSIZE] = {0};

  // wait事件
#define EPOLL_TIMEOUT_MS   -1  // -1 nonblock
  while(true){
    n_ready_event = epoll_wait(epoll_fd, recv_events, MAXNFD, EPOLL_TIMEOUT_MS);
    // int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
    // http://man7.org/linux/man-pages/man2/epoll_wait.2.html
    // timeout:
    //      如果-1非阻塞，如果有事件未处理，且为设置边缘触发，则一直触发事件。
    if(n_ready_event<0){
      tzError("epoll_wait()");
    }
    int iter = 0;
    for(iter=0; iter<n_ready_event; iter++){
      // 处理事件
      TzEventHandler *returned_event_handler = (TzEventHandler *)recv_events[iter].data.ptr;
      std::cout   << ">>>get event, handler fd:" << returned_event_handler->fd
                  << ", cnt:" << *(int*)returned_event_handler->ptr << std::endl;
      (*(int*)returned_event_handler->ptr)++;
      checEventType(recv_events[iter].events);

      // 如果是可读事件
      if(recv_events[iter].events & EPOLLIN){
        // 如果是像listen发出的监听请求
        if(returned_event_handler->fd==listenfd){
          // listener
          struct sockaddr_in clientaddr = {0};
          socklen_t client_sock_addr_len = sizeof(clientaddr);
          int tcp_socketfd = accept(listenfd, (struct sockaddr*)&clientaddr, &client_sock_addr_len);
          if (tcp_socketfd<0){
            tzError("accept()");
          }
          else{
            std::cout << "accept" << std::endl;
            std::cout << "incoming:" << inet_ntoa(clientaddr.sin_addr) << std::endl;
            // 为新的socket注册事件
            TzEventHandler socket_read_handler;
            socket_read_handler.fd = tcp_socketfd;   // event相关fd
            socket_read_handler.event_handler_func = read_handlerImpl;
            int cnt = 0;
            socket_read_handler.ptr = &cnt;

            struct epoll_event new_event = {0};
            new_event.events = EPOLLIN;
            new_event.data.ptr = (void*)&socket_read_handler;    // 指向处理事件的对象
            int epoll_ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tcp_socketfd, &new_event);
            if(epoll_ret==0) std::cout << "add event" << std::endl;
          }
        }
        else{
          // tcp peer msg
          returned_event_handler->event_handler_func();
#define BUF_SIZE 100
          char buf[BUF_SIZE];
          memset(buf, 0, BUF_SIZE);
          int ret = read(returned_event_handler->fd, buf, BUF_SIZE);
          // 注意：如果read的长度小于到达的数据，会留下剩余的数据再次触发IN evnent
          if(ret == 0){
            // connect closed
            std::cout << "TCP fd:" << returned_event_handler->fd << " disconnect." << std::endl;
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, returned_event_handler->fd, &recv_events[iter]);
            // 删除event，否则会不断报事件，其他处理方法？
            close(returned_event_handler->fd);
          }else{
            std::cout << "recv content:" << buf << std::endl;
            // server在收到时才被动应答，所以此时才设置发送event
            recv_events[iter].events = EPOLLOUT;   // 修改events类型，这样设置在有发送时不触发接收事件
            returned_event_handler->event_handler_func = send_handlerImpl;
            int ctl_ret = epoll_ctl(epoll_fd, EPOLL_CTL_MOD, returned_event_handler->fd, &recv_events[iter]);
            if(ctl_ret<0){
              // 使用error检查错误原因
              std::cout << "ctl_ret:" << ctl_ret << " errno:";
              std::cout << errno << std::endl;
            }
          }
        }
      }
        // 如果是可写事件
      else if(recv_events[iter].events & EPOLLOUT){
        returned_event_handler->event_handler_func();
        int write_ret = write(returned_event_handler->fd, "get one msg", 11);

        recv_events[iter].events = EPOLLIN;   // 修改events类型，这样设置在有接受时不触发发送事件
        returned_event_handler->event_handler_func = read_handlerImpl;
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, returned_event_handler->fd, &recv_events[iter]);
      }else{
        std::cout << "unknown event" << std::endl;
      }
    }
  } // while true
  return 0;
}