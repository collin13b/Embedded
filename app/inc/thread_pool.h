#ifndef __THREAD_POOL_H
#define __THREAD_POOL_H
#include <vector>
#include <thread>
#include <iostream>
#include "Safequeue.h"
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <future>
#include <exception>
#include <yolov5s.h>
#include <queue>
#include "Safequeue.h"
#include <stop_token>
#include <pthread.h>
#include <sched.h>
#include "post_process.h"
using namespace std;

struct ProcessData
{
    cv::Mat processed_frame;
    result_group results;
};

class ThreadPool {
public:
    ThreadPool(const string &model_path, int num_threads);
    ~ThreadPool();
    bool init(const string &model_path,int num_threads);
    void worker(int worker_id);
    future<ProcessData> submit_task(int index,cv::Mat &img);
private:
    atomic<bool> stop_flag{false};
    atomic<bool> run_flag{false};

    vector<thread> threads;
    mutex task_mutex;
    condition_variable_any task_cond;
    queue<packaged_task<ProcessData()>> tasks;
    vector<std::unique_ptr<yolov5s>> yolo_groups;
};    

#endif // __THREAD_POOL_H
