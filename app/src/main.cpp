#include <iostream>
#include <vector>
#include <ranges>

#include <string>
#include <opencv4/opencv2/core.hpp>
#include <opencv4/opencv2/highgui.hpp>
#include <thread>
#include <filesystem>
#include "Safequeue.h"
#include "yolov5s.h"
#include "thread_pool.h"
using namespace std;
using namespace cv; 
static string video_path = {"/home/cat/linux_Ai/test.mp4"};
static string video_output_path = {"/home/cat/linux_Ai/output.mp4"};
static string model_path = {"/home/cat/linux_Ai/model/yolov5s.rknn"};
static string label_path = {"/home/cat/linux_Ai/model/coco_80_labels_list.txt"};

static size_t MAX_CONCURRENT_FRAMES = 32;
struct FramePacket
{
    int frame_id;
    cv::Mat frame;
};
static SafeQueue<FramePacket> read_queue(1000);
static SafeQueue<FramePacket> write_queue(1000);
atomic<bool> read_finish{false};
atomic<bool> process_finish{false};
atomic<bool> write_finish{false};   

extern void bind_self_to_cores(const std::vector<int>& target_cores);
static void Read_func(cv::VideoCapture &cap)
{
    
    bind_self_to_cores({0,1,2,3}); //绑定读取线程到核心0-3    
    
    int frame_id = 0;
    while(1)
    {
        Mat img ;
        if(!cap.read(img))
        {
            cout<<("[READQUEUE]:video read failed or end of video\n");
            break;
        }
        FramePacket frame_pkt = {frame_id++,move(img)};
        read_queue.enqueue(move(frame_pkt));
        if(frame_id % 100 == 0)
            cout<<"Read frame id: "<<frame_id<<endl;
    }
    read_finish.store(true);
    cout<<"Read thread finished\n";

}
static void Process_func(ThreadPool &thread_pool)
{
    int next_frame_id = 0;
    FramePacket pending_pkt;
    bool has_pending = false;

    size_t RING_BUFFER_SIZE = MAX_CONCURRENT_FRAMES * 2;
    std::vector<std::future<ProcessData>> ring_buffer(RING_BUFFER_SIZE);
    while(true)
    {
        if(!has_pending)
        {
            if(!read_queue.isEmpty())
            {
                if(read_queue.dequeue(pending_pkt))
                {
                    has_pending = true;
                }
            }
            else if(read_finish.load())
            {
                bool all_tasks_done = true;
                size_t target_slot = next_frame_id % RING_BUFFER_SIZE; 
                if(ring_buffer[target_slot].valid())    all_tasks_done = false;
                if(all_tasks_done) break;
            }
        }
        if(has_pending)
        {
            size_t pending_slot = pending_pkt.frame_id % RING_BUFFER_SIZE;
            if(!ring_buffer[pending_slot].valid())
            {
                ring_buffer[pending_slot] = thread_pool.submit_task(pending_pkt.frame_id,pending_pkt.frame);
                has_pending = false;
            }
            else
            {}
        }

        size_t target_slot = next_frame_id % RING_BUFFER_SIZE;
        if(ring_buffer[target_slot].valid())
        {
            if(ring_buffer[target_slot].wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                ProcessData result = ring_buffer[target_slot].get();
                FramePacket temp_pkt = {next_frame_id,move(result.processed_frame)};
                write_queue.enqueue(move(temp_pkt));
                next_frame_id++;
                if(next_frame_id % 100 == 0)
                    cout<<"Process frame id: "<<next_frame_id<<endl;
            }
        }
        else
        {
            if(read_queue.isEmpty())
                this_thread::sleep_for(chrono::milliseconds(1));
            else if(has_pending)
                this_thread::sleep_for(chrono::milliseconds(1));
        }
    }
    process_finish.store(true);
    cout<<"Process thread finished\n";
}
static void Write_func(cv::VideoWriter &writer)
{
    bind_self_to_cores({0,1,2,3});
    FramePacket frame_pkt;
    Mat temp_img;
    int frame_write_id = 0;
    while (1)
    {
        if(!write_queue.dequeue(frame_pkt))
        {
            this_thread::sleep_for(chrono::milliseconds(1));
            continue;
        }
        if(!frame_pkt.frame.empty())
        {
            temp_img = frame_pkt.frame;
            //处理图像
            // writer.write(move(temp_img));
            frame_write_id++;
            if(frame_write_id % 100 == 0)
                cout<<"Write frame id: "<<frame_write_id<<endl;
        }
        if(process_finish.load() && write_queue.isEmpty())
        {
            
            break;
        }

    }
    writer.release();
    write_finish.store(true);
    cout<<"Write thread finished\n";
    
}
int main() {
        auto start = chrono::high_resolution_clock::now();
        cv::Mat img;
        img = cv::imread("/home/cat/linux_Ai/img2.png",cv::IMREAD_COLOR);
        cv::imwrite("/home/cat/linux_Ai/write.png",img);
        
        cv::VideoCapture cap(video_path);
        // string gst_pipeline = "filesrc location=" + video_path + " ! qtdemux ! h264parse ! mppvideodec ! videoconvert ! appsink sync=false";

        // cv::VideoCapture cap(gst_pipeline, cv::CAP_GSTREAMER);

        if(!cap.isOpened()) {
            cout << "GStreamer open failed! Check if gstreamer/opencv is installed correctly." << endl;
            // 回退到软解码试试
            cap.open(video_path);
        }
        if(!cap.isOpened())
        {
            cout<<"video open failed\n";
            return -1;
        }
        
        int video_fps = static_cast<int>(cap.get(cv::CAP_PROP_FPS));
        int height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
        int width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
        //H264
        cv::VideoWriter writer(video_output_path,cv::VideoWriter::fourcc('H','2','6','4'),video_fps,cv::Size(width,height));
        ThreadPool thread_pool(model_path,3);
        
        // while(1);
        thread Read_T(Read_func,std::ref(cap));
        thread Process_T(Process_func,ref(thread_pool));
        thread Write_T(Write_func,std::ref(writer));

        Read_T.join();
        Process_T.join();
        Write_T.join();

        cap.release();

        read_queue.stop();
        write_queue.stop();

        auto end = chrono::high_resolution_clock::now();
        auto duration_time = chrono::duration_cast<chrono::milliseconds>(end - start).count();
        cout<<"duration time cast "<<duration_time<<" ms"<<endl;
        return 0;
}
