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
using namespace std;
using namespace cv; 
static string video_path = {"/home/cat/linux_Ai/video.mp4"};
static string video_output_path = {"/home/cat/linux_Ai/output.mp4"};
static string model_path = {"/home/cat/linux_Ai/model/yolov5s.rknn"};
static string label_path = {"/home/cat/linux_Ai/model/coco_80_labels_list.txt"};
yolov5s yolo(model_path.c_str(),3);
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


static void Read_func(cv::VideoCapture &cap)
{
    
        
    
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
static void Process_func()
{
    FramePacket frame_pkt;
    Mat temp_img;
    int frame_process_id = 0;
    while (1)
    {
        if(read_finish.load() && read_queue.isEmpty())
        {
            break;
        }

        if(!read_queue.isEmpty())
        {
            read_queue.dequeue(frame_pkt);
            //处理图像
            temp_img = frame_pkt.frame;
            yolo.inference_img(temp_img);
            frame_process_id++;
            if(frame_process_id % 100 == 0)
                cout<<"Process frame id: "<<frame_pkt.frame_id<<endl;
            write_queue.enqueue({frame_process_id,move(temp_img)});
        }
    }
    process_finish.store(true);
    cout<<"Process thread finished\n";
}
static void Write_func(cv::VideoWriter &writer)
{
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
            writer.write(move(temp_img));
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

        
        // while(1);
        thread Read_T(Read_func,std::ref(cap));
        thread Process_T(Process_func);
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
