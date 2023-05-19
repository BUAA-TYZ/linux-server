// C system headers
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
// C++ system headers
#include <cstdio>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
// .h files in this project
#include "http_conn.h"
#include "threadpool.h"

const int MAX_FD = 65536;
const int MAX_EVENT_NUMBER = 10000;
const int TIMESLOT = 1;

static time_wheel timer_wheel;
static int pipefd[2];

extern int addfd (int epollfd, int sockfd, bool one_shot);
extern int removefd (int epollfd, int sockfd);
extern int setnonblock (int sockfd);

void sig_handler(int sig) {
    // unify the source of signals
    // write it to the pipefd[1]
    int save_errno = errno;
    send(pipefd[1], (char*) &sig, sizeof(sig), 0);
    errno = save_errno;
}
void addsig(int sig, void(handler)(int), bool restart = true) {
    struct sigaction sa{};
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    if (restart) {
        sa.sa_flags |= SA_RESTART;					// 重启动因信号被中断的系统调用
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, nullptr) != -1);
}

void show_error(int connfd, const char* info) {
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}
void timer_handler() {
    timer_wheel.tick();
    alarm(5 * TIMESLOT);
}
void cb_func(http_conn* user_data) {
    // Close the client
    printf("Close fd %d\n", user_data->sockfd);
    user_data->close_conn();
}

int main (int argc, char* argv[]) {
    if (argc <= 2) {
        printf("Usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }
    const char* ip = argv[1];
    int port = atoi(argv[2]);

    addsig(SIGPIPE, SIG_IGN);           //ignore the SIGPIPE

    threadpool<http_conn> *pool = nullptr;
    try {
        pool = pool->Getinstance();                // Singleton
    }
    catch (...) {                                  // catch all errors
        return 1;
    }

    auto users = new http_conn[MAX_FD];
    assert(users);
    http_conn::user_count = 0;

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);
    struct linger tmp = {1, 0};
    setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

    int ret = 0;
    struct sockaddr_in address{};
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret >= 0);

    ret = listen(listenfd, 5);
    assert(ret >= 0);

    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    addfd(epollfd, listenfd, false);
    http_conn::epollfd = epollfd;

    socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    setnonblock(pipefd[1]);
    addfd(epollfd, pipefd[0], false);           //monitor by main thread
    addsig(SIGALRM, sig_handler);
    addsig(SIGTERM ,sig_handler);
    alarm(TIMESLOT);
    bool stop_server = false;
    bool timeout = false;
    while (!stop_server)
    {
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        if ((number < 0) && (errno != EINTR)) {
            printf( "epoll failure\n" );
            break;
        }

        for ( int i = 0; i < number; i++ ) {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd) {
                struct sockaddr_in client_address{};
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *) &client_address, &client_addrlength);
                if (connfd < 0) {
                    printf("errno is: %d\n", errno);
                    continue;
                }
                if (http_conn::user_count >= MAX_FD) {
                    show_error(connfd, "Internal server busy");
                    continue;
                }
                printf("connecting...\n");
                printf("User: %d connected\n", connfd);
                tw_timer* timer = timer_wheel.add_timer(8*TIMESLOT);
                users[connfd].init(connfd, client_address, timer);

                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
            } else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
                char msg[1024];
                int ret = recv(sockfd, &msg, sizeof(msg), 0);
                if (ret <= 0)
                    continue;
                else {
                    for (int j = 0; j < ret; ++j) {
                        switch(msg[j]) {
                            case SIGALRM:
                                timeout = true;
                                break;
                            case SIGTERM:
                                stop_server = true;
                                break;
                        }
                    }
                }
            }
            // EPOLLRDHUP: client closes the connection
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                printf("Close %d cause some reasons\n", sockfd);
                tw_timer* timer = users[sockfd].timer;
                cb_func(&users[sockfd]);
                    if(timer) {
                        timer_wheel.del_timer(timer);
                    }
            } else if (events[i].events & EPOLLIN) {
                printf("User: %d reading...\n", sockfd);
                tw_timer* timer = users[sockfd].timer;
                if (users[sockfd].read()) {
                    if (timer) {
                        timer_wheel.del_timer(timer);
                        tw_timer *pTimer = timer_wheel.add_timer(30 * TIMESLOT);
                        pTimer->user_data = &users[sockfd];
                        pTimer->cb_func = cb_func;
                        users[sockfd].timer = pTimer;
                    }
                    pool->append(users + sockfd);
                }
                else {
                    cb_func(&users[sockfd]);
                    if (timer)
                        timer_wheel.del_timer(timer);
                }

            } else if (events[i].events & EPOLLOUT) {
                printf("User: %d writing...\n", sockfd);
                tw_timer* timer = users[sockfd].timer;
                if (!users[sockfd].write()) {
                    cb_func(&users[sockfd]);
                    if (timer) {
                        timer_wheel.del_timer(timer);
                    }
                }
            }
            else
            {}
        }
        if(timeout) {
            timeout = false;
            timer_handler();
        }
    }
    close(pipefd[0]);
    close(pipefd[1]);
    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool;
    return 0;
}
