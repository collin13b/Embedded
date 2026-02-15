#include "thread_pool.h"
ThreadPool::ThreadPool(const string &model_path, int num_threads) {
   if(!init(model_path,num_threads))
        run_flag = true;
}
extern void bind_self_to_cores(const std::vector<int>& target_cores);
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

bool ThreadPool::init(const string &model_path,int num_threads)
{
    if(num_threads <= 0)
        num_threads = 3;
    for(int i = 0; i < num_threads; ++i)
    {
        auto yolo = std::make_unique<yolov5s>(model_path.c_str(), i % 3);
        yolo_groups.emplace_back(move(yolo));
    }
    
    for(int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(&ThreadPool::worker,this,i);
    }
    cout<<"init thread pool successful\n";
    return true;
}



void ThreadPool::worker(int worker_id){
    cout<<"worker "<<worker_id<<" is running\n";
        //绑定线程到特定核心
    vector<int> cores_to_bind ={4,5,6,7};
    bind_self_to_cores(cores_to_bind);
    while(!run_flag.load())
    {
        std::packaged_task<ProcessData()> task;
        {
            //从任务队列中取出一个任务
            std::unique_lock lock(task_mutex);
            task_cond.wait(lock,[this]{return !tasks.empty() || run_flag.load();});
            //如果收到停止信号，退出
            if(run_flag.load())
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
            auto &yolo = yolo_groups[index % yolo_groups.size()];
            yolo->inference_img(const_cast<cv::Mat&>(img));
            result.processed_frame = img;
        }
        catch(const std::exception& e)
        {
            std::cerr << e.what() << '\n';
        }
        
        return result;
    }
    );
    std::future<ProcessData> future_result = task.get_future();
    {
        unique_lock lock(task_mutex);
        tasks.emplace(move(task));
    }
    task_cond.notify_one();
    return future_result;
}
