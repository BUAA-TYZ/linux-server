//
// Created by tyz on 23-3-30.
//

#ifndef WEBSERVER_HTTP_CONN_H
#define WEBSERVER_HTTP_CONN_H
// C system headers
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
// C++ system headers
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <csignal>
#include <iostream>

class tw_timer;
class http_conn{
public:
    static const int FILENAME_LEN = 200;    //maxlen of the filename
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    enum METHOD{GET=0, POST, HEAD, PUT,		//only support GET METHOD
            DELETE, TRACK, OPTIONS, CONNECT, PATCH};
    enum CHECK_STATE{CHECK_STATE_REQUESTLINE=0,
            CHECK_STATE_HEADER,
            CHECK_STATE_CONTENT};
    enum HTTP_CODE{NO_REQUEST, GET_REQUEST, BAD_REQUEST,
                    NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST,
                    INTERNAL_ERROR, CLOSED_CONNECTION};
    enum LINE_STATUS{LINE_OK=0, LINE_BAD, LINE_OPEN};
    int sockfd;
    sockaddr_in clnt_adr;
    tw_timer* timer;

public:
    [[maybe_unused]] http_conn() = default;
    [[maybe_unused]] ~http_conn() = default;

    void init(int sockfd, const sockaddr_in& addr, tw_timer*);
    void close_conn(bool real_close = true);
    void process();
    bool read();
    bool write();

private:
    void init();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);

    HTTP_CODE parse_request_line(char* text);
    HTTP_CODE parse_headers(char* text);
    HTTP_CODE parse_content(char* text);
    HTTP_CODE do_request();
    char* get_line(){
        return read_buf + start_line;
    }
    LINE_STATUS parse_line();

    // functions for responding HTTP
    void unmap();
    bool add_response(const char* format, ...);
    bool add_content(const char* content);
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int epollfd;
    static int user_count;

private:
    char read_buf[READ_BUFFER_SIZE];    // buffer of reading
    int read_idx;                       // next to read
    int check_idx;                      // deal with it now
    int start_line;
    char write_buf[WRITE_BUFFER_SIZE];
    int write_idx;

    CHECK_STATE check_state;
    METHOD method;

    char real_file[FILENAME_LEN];       // real name: doc_root+url
    char* url;
    char* version;
    char* host;
    int content_length;                 // length of the HTTP request
    bool linger = true;                 // whether to stay connected

    char* file_address;                 // position of the file
    struct stat file_stat;              // state of the file

    struct iovec iv[2];
    int iv_count;
};

class tw_timer{
public:
    void (*cb_func)(http_conn*);        // when timer expires, this function is invoked
    http_conn* user_data;
    tw_timer (int rot, int sl): prev(nullptr), next(nullptr)
            ,rotation(rot), slot(sl) {}
    tw_timer* prev;
    tw_timer* next;
    int rotation;                    // nums of the round
    int slot;                        // index of slot
};

class time_wheel{
public:
    time_wheel (): curslot(0) {
        for(auto& slot : slots)
            slot = nullptr;
    }
    ~time_wheel() {
        for(auto & slot : slots){
            tw_timer* tmp = slot;
            while(tmp){
                tmp = tmp->next;
                delete slot;
                slot = tmp;
            }
        }
    }

    tw_timer* add_timer(int timeout) {
        if (timeout<=0)
            return new tw_timer(0, 0);
        int tick;
        if (timeout < TI)
            tick = 1;
        else
            tick = timeout / TI;
        int whichrot = tick / Numslot;
        int whichslot = (curslot + tick % Numslot) % Numslot;
        auto* tmp = new tw_timer(whichrot, whichslot);
        if (!slots[whichslot]) {
            // this node is the head of the list
            printf("Add timer rotation is %d, "
                   "slot is %d, curslot is %d, \n", whichrot, whichslot, curslot);
            slots[whichslot] = tmp;
        } else {
            tmp->next = slots[whichslot];
            slots[whichslot] = slots[whichslot]->prev = tmp;
        }
        return tmp;
    }
    void del_timer (tw_timer* timer) {
        if (!timer)
            return;
        // if this is the head of the list
        int slot = timer->slot;
        if (timer == slots[slot]) {
            slots[slot] = timer->next;
            if (slots[slot])
                slots[slot]->prev = nullptr;
            delete timer;
        } else {
            //this is not the head, so it has the prev node
            timer->prev->next = timer->next;
            if (timer->next)
                timer->next->prev = timer->prev;
            delete timer;
        }
    }
    void tick() {
        // delete timers on curslot
        tw_timer* tmp = slots[curslot];
        std::cout << "Current slot is " << curslot << std::endl;
        while (tmp) {
            if (tmp->rotation) {
                tmp->rotation--;
                tmp = tmp->next;
            } else {
                tmp->cb_func(tmp->user_data);
                if (tmp == slots[curslot]) {
                    slots[curslot] = tmp->next;
                    if (slots[curslot])
                        slots[curslot]->prev = nullptr;
                    delete tmp;
                    tmp = slots[curslot];
                } else {
                    tmp->prev->next = tmp->next;
                    if (tmp->next)
                        tmp->next->prev = tmp->prev;
                    tw_timer* tmp2 = tmp->next;
                    delete tmp;
                    tmp = tmp2;
                }
            }
        }
        curslot = (++curslot) % Numslot;
    }
private:
    static const int Numslot = 60;
    static const int TI = 1;                            // period betweei slots
    int curslot;
    tw_timer* slots[Numslot];
};

#endif //WEBSERVER_HTTP_CONN_H
