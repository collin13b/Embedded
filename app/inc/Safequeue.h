#ifndef __SAFEQUEUE_H
#define __SAFEQUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <atomic>
#include <chrono>

using namespace std;

template <typename T>//传入函数的可以自动推导，在外面必须指定类型

class SafeQueue {
public:
    SafeQueue(int Max_size): max_size(Max_size) {};
    ~SafeQueue(){};
    bool enqueue(const T &t )
    {
        unique_lock<mutex> lock(q_mutex);//使用条件变量必须使用unique_lock
        bool status =  q_cond.wait_for(lock, chrono::milliseconds(100) ,
            [this]{return stop_flag.load() || q.size() < max_size; });//等待条件变量，直到队列不满
        if(stop_flag.load() || !status ) return false;
        q.push(t);
        q_cond.notify_all();//通知可能等待的线程
        return true;
    }
    bool dequeue(T & t)
    {
        unique_lock<mutex> lock(q_mutex);
        q_cond.wait(lock,[this]{return !q.empty(); });//等待条件变量，直到队列不为空
        if(stop_flag.load() && q.empty()) return false;
        t = q.front();
        q.pop();
        q_cond.notify_one();//通知可能等待的线程
        return true;
    }
    bool isEmpty () const
    {
        unique_lock<mutex> lock(q_mutex);
        return q.empty();
    }
    size_t size() const
    {
        unique_lock<mutex> lock(q_mutex);
        return q.size();
    }
    void stop()
    {
        unique_lock<mutex> lock(q_mutex);
        stop_flag = true;
        q_cond.notify_all();
    }
private:
    atomic<bool> stop_flag{false};
    mutable mutex q_mutex;//lambda表达式中使用了this指针，所以mutex必须是mutable的，内部成员不会变
    condition_variable q_cond;
    size_t max_size;
    queue<T> q;
};


#endif // !__SAFEQUEUE_H
