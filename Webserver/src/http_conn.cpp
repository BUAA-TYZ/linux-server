//
// Created by tyz on 23-3-30.
//

#include "http_conn.h"
//state information of HTTP response
const char* ok_200_title = "OK";
const char* errno_400_title = "BAD_REQUEST";
const char* errno_400_form = "Your request has bad syntax\n";
const char* errno_403_title = "Forbidden";
const char* errno_403_form = "You do not have permission to get file from this server\n";
const char* errno_404_title = "Not Found";
const char* errno_404_form = "The request file was not found on this server\n";
const char* errno_500_title = "Internal Errno";
const char* errno_500_form = "There was an unusual problem\n";
//root directory
const char* doc_root = "/home/tyz/Desktop/C++-learning/linux-highperformance/Webserver/bin";

int setnonblock(int sockfd){
    int old_option = fcntl(sockfd, F_GETFL);
    fcntl(sockfd, F_SETFL, old_option | O_NONBLOCK);
    return old_option;
}
void addfd(int epollfd, int sockfd, bool oneshot){
    epoll_event event;
    event.data.fd = sockfd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if (oneshot) {
        event.events |= EPOLLONESHOT;                   // the sockfd only use by one thread
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, sockfd, &event);
    setnonblock(sockfd);
}
void removefd(int epollfd, int sockfd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, sockfd, nullptr);
    close(sockfd);
}

void modfd(int epollfd, int sockfd, int ev){
    epoll_event event;
    event.data.fd = sockfd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP | ev;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, sockfd, &event);
}

int http_conn::user_count = 0;
int http_conn::epollfd = -1;

void http_conn::close_conn(bool real_close) {
    if (real_close && sockfd != -1) {
        removefd(epollfd, sockfd);
        sockfd = -1;
        user_count--;
    }
}

void http_conn::init(int fd, const sockaddr_in& adr, tw_timer* timerr){
    sockfd = fd;
    clnt_adr = adr;
    timer = timerr;
    addfd(epollfd, fd, true);
    user_count++;

    init();
}

void http_conn::init(){
    check_state = CHECK_STATE_REQUESTLINE;
    linger = true;
    method = GET;
    url = nullptr;
    version = nullptr;
    content_length = 0;
    host = nullptr;
    start_line = 0;
    check_idx = read_idx = write_idx = 0;
    memset(read_buf, '\0', sizeof(read_buf));
    memset(write_buf, '\0', sizeof(write_buf));
    memset(real_file, '\0', sizeof(real_file));
}

http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for (; check_idx < read_idx; check_idx++) {
        temp = read_buf[check_idx];
        if (temp == '\r') {
            if (check_idx+1 == read_idx)
                return LINE_OPEN;
            else if (read_buf[check_idx+1] == '\n') {
                read_buf[check_idx++] = '\0';
                read_buf[check_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n') {
            if (check_idx > 1 && read_buf[check_idx-1] == '\r') {
                read_buf[check_idx-1] = '\0';
                read_buf[check_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

bool http_conn::read(){
    if (read_idx >= READ_BUFFER_SIZE)
        return false;
    int bytes_read = 0;
    while (true) {
        bytes_read = recv(sockfd, read_buf+read_idx, READ_BUFFER_SIZE-read_idx, 0);
        if (bytes_read == -1) {
            // in most situations, EAGAIN = EWOULDBLOCK except for some old versions of LINUX
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            // printf("User: %d errno\n", sockfd);
            return false;
        } else if (bytes_read == 0) {
            return false;
        }
        read_idx += bytes_read;
    }
    return true;
}

/*  GET /example.html HTTP/1.1
    Host: example.com
*/
http_conn::HTTP_CODE http_conn::parse_request_line(char *text) {
    url = strpbrk(text, " \t");               //search " \t"
    if (!url)
        return BAD_REQUEST;
    *url++ = '\0';
    char* method = text;
    if (strcasecmp(method, "GET") != 0) {      //ignore upper or lower
        return BAD_REQUEST;
    }
    printf("User: %d Method: %s\n", sockfd, method);
    url += strspn(url, " \t");
    version = strpbrk(url, " \t");
    if (!version)
        return BAD_REQUEST;
    *version++ = '\0';
    version += strspn(version, " \t");
    if (strcasecmp(version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(url, "http://", 7) == 0) {
        url+=7;
        url = strchr(url, '/');                 // search '/' after http://
    }
    if(!url || url[0] != '/')
        return BAD_REQUEST;
    check_state =  CHECK_STATE_HEADER;
    printf("change to state: CHECK_STATE_HEADER\n");
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers(char *text) {
    if (text[0] == '\0') {                // after parse_line(), \r\n -> \0
        if (content_length != 0) {
            check_state = CHECK_STATE_CONTENT;
            printf("change to state: CHECK_STATE_CONTENT\n");
            return NO_REQUEST;
        }
        // printf("process file\n");
        return GET_REQUEST;             // finish
    }
    else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "close") == 0)
            linger = false;
    }
    else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        content_length = atoi(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        host = text;
    }
    else {
        printf("unknown\n");
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_content(char *text) {     // actually, we don't care this
    if (read_idx >= content_length + check_idx) {
        text[content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = nullptr;
    while ((check_state == CHECK_STATE_CONTENT && line_status==LINE_OK) ||
            (line_status = parse_line()) == LINE_OK) {
        text = get_line();
        start_line = check_idx;
        printf("get 1 http line: %s\n", text);

        switch (check_state) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }
            case CHECK_STATE_HEADER: {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                else if (ret == GET_REQUEST)
                    return do_request();
                break;
            }
            case CHECK_STATE_CONTENT: {
                ret = parse_content(text);
                if(ret == GET_REQUEST)
                    return do_request();
                line_status = LINE_OPEN;
                break;
            }
            default:
                return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request() {
    strcpy(real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(real_file+len, url, FILENAME_LEN-len-1);
    if (stat(real_file, &file_stat) < 0)
        return NO_RESOURCE;
    if(!(file_stat.st_mode & S_IROTH))                  // have the permission?
        return FORBIDDEN_REQUEST;
    if(S_ISDIR(file_stat.st_mode))
        return BAD_REQUEST;
    int sockfd = open(real_file, O_RDONLY);
    file_address = (char *)mmap(nullptr, file_stat.st_size,
                                PROT_READ, MAP_PRIVATE, sockfd, 0);
    close(sockfd);
    return FILE_REQUEST;
}
void http_conn::unmap() {
    if (file_address) {
        munmap(file_address, file_stat.st_size);
        file_address = nullptr;
    }
}
bool http_conn::write(){
    int temp = 0;
    int bytes_have_send = 0;
    int bytes_to_send = write_idx;
    if (bytes_to_send == 0) {
        modfd(epollfd, sockfd, EPOLLIN);
        init();
        return true;
    }
    while (true) {
        temp = writev(sockfd, iv, iv_count);
        if (temp <= -1) {
            if (errno == EAGAIN) {
                modfd(epollfd, sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        if (bytes_to_send <= bytes_have_send) {
            unmap();
            if (linger) {
                init();
                modfd(epollfd, sockfd, EPOLLIN);
                return true;
            } else {
                modfd(epollfd, sockfd, EPOLLIN);
                return false;
            }
        }
    }
}

bool http_conn::add_response(const char *format, ...) {
    if (write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(write_buf + write_idx, WRITE_BUFFER_SIZE-1-write_idx,
                        format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE-1-write_idx))
        return false;
    write_idx += len;
    va_end(arg_list);
    return true;
}
bool http_conn::add_status_line(int status, const char *title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_length) {
    return add_content_length(content_length) &&
    add_linger() &&
    add_blank_line();
}
bool http_conn::add_content_length(int content_length) {
    return add_response("Content-Length: %d\r\n", content_length);
}
bool http_conn::add_linger() {
    return add_response("Connection: %s\r\n", (linger ? "keep-alive" : "close"));
}
bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content) {
    return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret) {
        case INTERNAL_ERROR: {
            add_status_line(500, errno_500_title);
            add_headers(strlen(errno_500_form));
            if (!add_content(errno_500_form))
                return false;
            break;
        }
        case BAD_REQUEST: {
            add_status_line(400, errno_400_title);
            add_headers(strlen(errno_400_form));
            if (!add_content(errno_400_form))
                return false;
            break;
        }
        case NO_RESOURCE: {
            add_status_line(404, errno_404_title);
            add_headers(strlen(errno_404_form));
            if (!add_content(errno_404_form))
                return false;
            break;
        }
        case FORBIDDEN_REQUEST: {
            add_status_line(403, errno_403_title);
            add_headers(strlen(errno_403_form));
            if (!add_content(errno_403_form))
                return false;
            break;
        }
        case FILE_REQUEST: {
            add_status_line(200, ok_200_title);
            if (file_stat.st_size != 0) {
                add_headers(file_stat.st_size);
                iv[0].iov_base = write_buf;
                iv[0].iov_len = write_idx;
                iv[1].iov_base = file_address;
                iv[1].iov_len = file_stat.st_size;
                iv_count = 2;
                return true;
            } else {
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        }
        default:
            return false;
    }
    iv[0].iov_base = write_buf;
    iv[0].iov_len = write_idx;
    iv_count = 1;
    return true;
}
void http_conn::process() {
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) {
        modfd(epollfd, sockfd, EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
    }
    modfd(epollfd, sockfd, EPOLLOUT);
}
