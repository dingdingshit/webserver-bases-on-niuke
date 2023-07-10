#include<iostream>
#include<stdio.h>
#include<string.h>
#include<stdlib.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<errno.h>
#include<fcntl.h>
#include<sys/epoll.h>
#include<signal.h>
#include<string>
#include<iostream>
#include"locker.h"
#include"threadpool.h"
#include"http_conn.h"
#include"lst_timer.h"

#define MAX_FD 65535    //最多的http连接
#define MAXEVENTS 10000  //最大的事件数
#define TIME_SLOT 5     //定时器触发时间片

static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd;
//添加信号捕捉
void addsig(int sig,void (handler)(int))
{
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler=handler;
    sigfillset(&sa.sa_mask);
    sigaction(sig,&sa,NULL);
}
void sig_handler(int sig)
{
    int save_errno=errno;
    int msg=sig;
    send(pipefd[1],(char*)&msg,1,0);
    errno=save_errno;
}

void timer_handler()
{
    //定时处理任务，实际就是调用tick()函数
    timer_lst.tick();
    //一次arlarm调用只会引起一次SIGALARM信号，所以重新设闹钟
    alarm(TIME_SLOT);
}

void cb_func(client_data* user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    //assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    std::cout<<"关闭连接:"<<user_data->sockfd<<std::endl;
}

//添加文件描述符到epoll中
//删除文件描述符从epoll中
//修改文件描述符从epoll中，主要是重置epolloneshot
//设置文件描述符非阻塞
extern void addfd(int epollfd,int fd,unsigned int events);
extern void removefd( int epollfd, int fd );
extern void modfd(int epollfd, int fd, unsigned int events);
extern void setnonblocking( int fd );



int main(int argc,char* argv[])
{
    if(argc<2)
    {
        printf("按照如下格式运行：%s port number\n",basename(argv[0])); 
    }

    addsig(SIGPIPE,SIG_IGN);
    addsig(SIGALRM,sig_handler);
    addsig(SIGTERM,sig_handler);

    threadpool<http_conn> * pool;
    try{
        pool=new threadpool<http_conn>;
    }
    catch(...)
    {
        exit(-1);
    } 

    //创建一个数组保存http_conn
    http_conn *user=new http_conn[MAX_FD];
    //创建一个数组保存util_timer
    client_data* client_users=new client_data[MAX_FD];

   // 创建监听套接字
    int listenfd=socket(PF_INET,SOCK_STREAM,0);
    
    //设置端口复用
    int reuse=1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    //绑定
    struct sockaddr_in address;
    memset(&address,0,sizeof(address));
    address.sin_family=AF_INET;
    address.sin_addr.s_addr=INADDR_ANY;
    address.sin_port=htons(atoi(argv[1]));
    bind(listenfd,(struct sockaddr*)&address,sizeof(address));


    //listen
    listen(listenfd,8);

    //创建epoll实例
    epollfd=epoll_create(5);
    epoll_event events[MAXEVENTS];
    addfd(epollfd,listenfd,EPOLLIN);
    http_conn:: m_epollfd=epollfd;

    // 创建管道
    int ret=socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);

    setnonblocking( pipefd[1] );
    addfd(epollfd, pipefd[0] ,EPOLLIN|EPOLLET);

    bool timeout=false;
    bool stop_server=false;

    alarm(TIME_SLOT);//定时，5秒后闹钟响
    
    while(!stop_server)
    {
        int num_events=epoll_wait(epollfd,events,MAXEVENTS,-1);
        if(num_events==-1)
        {
            if(errno==EINTR) //epoll_wait会被SIGALRM信号中断返回-1
            {
                continue;
            }
            std::cerr<<"epoll_wait failed ."<<std::endl;
            exit(-1);
    }
    for(int i=0;i<num_events;i++)
    {
        int sockfd=events[i].data.fd;
        if(events[i].data.fd==listenfd) //new 连接
        {
            struct sockaddr_in clientaddr;
            socklen_t clientaddrlength=sizeof(clientaddr);
            int connfd=accept(listenfd,(struct sockaddr*)&clientaddr,&clientaddrlength);
            if(connfd==-1)
            {
                std::cerr<<"accept error"<<std::endl;
            }
            if(http_conn::m_user_count>MAX_FD)
            {
                close(connfd);
                continue;
            }
            else
            {

                user[connfd].init(connfd,clientaddr);

                util_timer* timer=new util_timer;
                client_users[connfd].address=clientaddr;
                client_users[connfd].sockfd=connfd;
                client_users[connfd].timer=timer;
                time_t cur=time(NULL);
                timer->expire=cur+3*TIME_SLOT;
                timer->cb_func=cb_func;
                timer->user_data=&client_users[connfd];
                timer_lst.add_timer(timer);
            }

        }
        else if(( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN ) ) 
        {
                    // 处理信号
                int sig;
                char signals[1024];
                int ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 ) {
                    continue;
                } else if( ret == 0 ) {
                    continue;
                } else  {
                    for( int i = 0; i < ret; ++i ) {
                        switch( signals[i] )  {
                            case SIGALRM:
                            {
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                            }
                        }
                    }
        }
        else if(events[i].events&(EPOLLHUP|EPOLLRDHUP|EPOLLERR))
        {

            user[sockfd].close_conn();
        }
        else if(events[i].events&EPOLLIN)
        {
            util_timer* timer=client_users[sockfd].timer;
            //一次性读取数据成功
            if(user[sockfd].read())
            {
                pool->append(user+sockfd);
                if(timer)
                {
                    time_t cur = time( NULL );
                    timer->expire = cur + 3 * TIME_SLOT;
                    timer_lst.adjust_timer(timer);
                    printf("timer adjust\n");
                }

            }
            else
            {
                timer_lst.del_timer(timer);
                user[sockfd].close_conn();
            }
            
        }
        else if(events[i].events&EPOLLOUT)
        {
            //一次性写完所有数据
            bool ret=user[sockfd].write();
            if(!ret)
            {
                user[sockfd].close_conn();
            }
        }
        
    }
    if( timeout ) {
        timer_handler();
        timeout = false;
    }
    }
    close(pipefd[0]);
    close(pipefd[1]);
    close(epollfd);
    close(listenfd);
    delete [] user;
    delete [] client_users;
    delete pool;
    return 0;

}