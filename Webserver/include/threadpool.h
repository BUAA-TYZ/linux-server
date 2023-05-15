//
// Created by tyz on 23-3-30.
//

#ifndef WEBSERVER_THREADPOOL_H
#define WEBSERVER_THREADPOOL_H

// C++ system headers
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <list>
#include <mutex>
#include <thread>
#include <vector>

template<typename T>
class threadpool {
public:
    static threadpool* Getinstance (int para_max_request=10000) {
        std::once_flag init_flag;
        std::call_once(init_flag, [&] {
            uniqueinstance = new threadpool(para_max_request);
        });
        return uniqueinstance;
    }
    ~threadpool();
    bool append(T* request);

private:
    explicit threadpool(int);
    static threadpool* uniqueinstance;
    void run();
private:
    unsigned int thread_num;                           // number of threads in the pool
    int max_request;                                   // max of requests
    std::vector<std::thread> threads;                  // array of threads
    std::list<T*> workqueue;
    std::mutex queuelocker;
    std::condition_variable queuestat;
    bool stop;
};
template<typename T>
threadpool<T>* threadpool<T>::uniqueinstance = nullptr;

template<typename T>
bool threadpool<T>::append(T* request) {
    {
        std::lock_guard<std::mutex> guard1(queuelocker);
        if (workqueue.size() > max_request) {
            queuelocker.unlock();
            return false;
        }
        workqueue.emplace_back(request);
    }
    queuestat.notify_one();
    return true;
}

template<typename T>
threadpool<T>::threadpool(int n1):
    thread_num(std::thread::hardware_concurrency()), max_request(n1), stop(false)
{
    if (n1 <= 0)
        throw std::exception();

    try {
        for (int i = 0; i < thread_num; ++i) {
            printf("create the %dth thread\n", i);
            threads.emplace_back(&threadpool::run, this);
        }
    } catch(...) {
        stop = true;
        throw std::runtime_error("Create thread fails !");
    }
    for (int i = 0; i < thread_num; ++i)
        threads[i].detach();
}

template<typename T>
threadpool<T>::~threadpool() {
   stop = true;
   queuestat.notify_all();
}

template<typename T>
void threadpool<T>::run() {
    while (!stop) {
    	std::unique_lock<std::mutex> guard1(queuelocker);
        queuestat.wait(guard1);
        if(workqueue.empty()){
            continue;
        }
        T* request = workqueue.front();
        workqueue.pop_front();
        if (!request) {
            continue;
        }
        request->process();
    }
}
#endif //WEBSERVER_THREADPOOL_H
