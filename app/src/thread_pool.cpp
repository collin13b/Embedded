#include "thread_pool.h"
ThreadPool::ThreadPool(const char *model_path, int num_threads) {
   run_flag = true;
   if(!init(model_path,num_threads))
        run_flag = false;
}

ThreadPool::~ThreadPool() {
    run_flag = true;
    task_cond.notify_all();

    for(auto &t : threads)
    {
        if(t.joinable())
            t.join();
    }
    cout<<"ThreadPool destroyed\n";
}

bool ThreadPool::init(const char *model_path,int num_threads)
{
    if(num_threads <= 0)
        num_threads = 3;
    
    for(int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(&ThreadPool::worker,this,i);
    }
    cout<<"init thread pool successful\n";
    return true;
}
void ThreadPool::worker(int worker_id){
    cout<<"worker "<<worker_id<<" is running\n";
    while(!run_flag.load())
    {
        std::packaged_task<ProcessData()> task;
        {
            //从任务队列中取出一个任务
            std::unique_lock lock(task_mutex);
            task_cond.wait(lock,[this]{return !tasks.empty() || !run_flag.load();});//如果没有任务，线程就等待
            //如果没有任务，线程就等待
            if(!run_flag.load())
            {
                cout<<"worker "<<worker_id<<" is stopping\n";
                break;
            }
            //get task
            if(!tasks.empty())
            {
                task = move(tasks.front());
                tasks.pop();
            }
        }
        //取到任务后执行
        if(task.valid())
        {
            task();
        }
    }
}
future<ProcessData> ThreadPool::submit_task(int index,cv::Mat &img)
{
    //打包任务为 std::packaged_task<ProcessResult()>
    std::packaged_task<ProcessData()> task([this,index,img]{
        ProcessData result;
        try
        {
            auto yolo = yolo_groups[index % yolo_groups.size()];
        }
        catch(const std::exception& e)
        {
            std::cerr << e.what() << '\n';
        }
        

    }
    );

    return task.get_future();
}
