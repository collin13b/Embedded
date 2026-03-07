#include <iostream>
#include <vector>
#include <ranges>
#include "post_process.h"
#include <string>
#include <opencv4/opencv2/core.hpp>
#include <opencv4/opencv2/highgui.hpp>
#include <thread>
#include <filesystem>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include "Safequeue.h"
#include "yolov5s.h"
#include "thread_pool.h"
#include "mpp.h"
#include "ZLMidea.h"
using namespace std;
using namespace cv; 
static string video_path = {"/home/cat/linux_Ai/test.mp4"};
static string video_output_path = {"/home/cat/linux_Ai/output.avi"};
static string model_path = {"/home/cat/linux_Ai/model/yolov5s.rknn"};
static string label_path = {"/home/cat/linux_Ai/model/coco_80_labels_list.txt"};
static size_t MAX_CONCURRENT_FRAMES = 32;
struct FramePacket
{
    int frame_id;
    cv::Mat frame;
    cv::Mat nv12_frame;
};
struct Buffer {
    void* start;
    size_t length;
};
struct Buffer_dma {
    void *start;
    int exprot_fd;
    size_t length;
};
static SafeQueue<FramePacket> read_queue(1000);
static SafeQueue<FramePacket> write_queue(1000);
atomic<bool> read_finish{false};
atomic<bool> process_finish{false};
atomic<bool> write_finish{false};   

static void nv12_to_bgr(Mat &img,Mat &cvt_img,int index,vector<Buffer> &buffer)
{
    int img_width = img.cols;
    int img_height = img.rows;
    rga_buffer_t src_rga = wrapbuffer_virtualaddr(buffer[index].start,img_width,img_height,RK_FORMAT_YCbCr_420_SP);//nv12;
    rga_buffer_t dst_rga = wrapbuffer_virtualaddr(cvt_img.data,img_width,img_height,RK_FORMAT_BGR_888);
    
    int rga_ret = imcvtcolor(src_rga,dst_rga,RK_FORMAT_YCbCr_420_SP,RK_FORMAT_BGR_888);
    if(rga_ret != IM_STATUS_SUCCESS)
    {
        cerr << "[rga error]"<<endl;
        return;
    }
}
static void nv12_to_bgr_dma(int img_width,int img_height,Mat &cvt_img,int fd,vector<Buffer_dma> &buffer)
{
    if (cvt_img.empty() || cvt_img.cols != img_width || cvt_img.rows != img_height) {
        cvt_img.create(img_height, img_width, CV_8UC3); 
    }

    rga_buffer_t src_rga = wrapbuffer_fd(fd,img_width,img_height,RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst_rga = wrapbuffer_virtualaddr(
        cvt_img.data, 
        img_width, 
        img_height, 
        RK_FORMAT_BGR_888
    );
    
    int rga_ret = imcvtcolor(src_rga,dst_rga,RK_FORMAT_YCbCr_420_SP,RK_FORMAT_BGR_888);
    if(rga_ret != IM_STATUS_SUCCESS)
    {
        cerr << "[rga error]"<<endl;
        return;
    }
}

extern void bind_self_to_cores(const std::vector<int>& target_cores);
static void Read_func(int fd)
{
    int frame_id = 0;
    int ret;
    
    // ================= 1. 设置格式 (MPLANE) =================
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; // 【关键】
    fmt.fmt.pix_mp.width = 1632;
    fmt.fmt.pix_mp.height = 1224;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    fmt.fmt.pix_mp.num_planes = 1; // NV12 作为一个整体平面
    
    ret = ioctl(fd, VIDIOC_S_FMT, &fmt);
    if(ret < 0) {
        perror("[V4L2 ERROR] set fmt failed");
        read_finish.store(true);
        return; // 【防弹】必须退出！
    }

    // ================= 2. 申请缓冲区 (MPLANE) =================
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 8;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; // 【关键】
    req.memory = V4L2_MEMORY_MMAP;
    
    ret = ioctl(fd, VIDIOC_REQBUFS, &req);
    if(ret < 0) {
        perror("[V4L2 ERROR] request buffer failed");
        cerr << "-> 请检查：1.是否有其他程序占用相机 2.CMA内存是否不足" << endl;
        read_finish.store(true);
        return; // 【防弹】必须退出！
    }

    // ================= 3. 内存映射 =================
    vector<Buffer_dma> buffer(req.count);
    for(int i = 0; i < req.count; ++i)
    {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[1]; // 【关键】平面数组
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = 1;

        ret = ioctl(fd, VIDIOC_QUERYBUF, &buf);
        if(ret < 0) {
            perror("[V4L2 ERROR] query buffer failed");
            return;
        }

        buffer[i].length = buf.m.planes[0].length;
        // buffer[i].start = mmap(NULL, buf.m.planes[0].length, 
        //                        PROT_READ | PROT_WRITE, MAP_SHARED, 
        //                        fd, buf.m.planes[0].m.mem_offset);
        

        if (buffer[i].start == MAP_FAILED) {
            perror("[V4L2 ERROR] mmap failed");
            return;
        }
        struct v4l2_exportbuffer export_buffer;
        memset(&export_buffer,0,sizeof(export_buffer));
        export_buffer.index = i;
        export_buffer.flags = O_RDWR | O_CLOEXEC;
        export_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        export_buffer.plane = 0;

        if(ioctl(fd,VIDIOC_EXPBUF,&export_buffer) < 0)
        {
            perror("[v4l2 error] export buffer failed");
            return;
        }
        buffer[i].exprot_fd = export_buffer.fd;

        ioctl(fd, VIDIOC_QBUF, &buf); // 入队
    }

    // ================= 4. 开启数据流 =================
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; // 【关键】
    ret = ioctl(fd, VIDIOC_STREAMON, &type);
    if(ret < 0) {
        perror("[V4L2 ERROR] stream on failed");
        return;
    }

    struct pollfd pool_fds[1];//poll机制
    pool_fds[0].fd = fd;
    pool_fds[0].events = POLLIN;    

    // ================= 5. 抓图循环 =================
    while(!read_finish.load()) // 增加外部退出控制
    {
        ret = poll(pool_fds, 1, 5000);
        if(ret == 0) {
            cout << "[WARN] Poll timeout, retrying..." << endl;
            continue;
        }
        
        struct v4l2_buffer buf;
        struct v4l2_plane planes[1];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = 1;

        // 出队
        ret = ioctl(fd, VIDIOC_DQBUF, &buf);
        if(ret < 0) {
            perror("[V4L2 ERROR] DQBUF failed");
            break;
        }

        // 处理图像
        // Mat nv12_img(1224 * 3 / 2,1632,CV_8UC3);
        // memcpy(nv12_img.data,buffer[buf.index].start,buffer[buf.index].length);

        Mat bgr_img;
        int cur_fd = buffer[buf.index].exprot_fd;
        nv12_to_bgr_dma(1632,1224,bgr_img,cur_fd,buffer);
        // nv12_to_bgr(bgr_img,bgr_img,buf.index,buffer);
        // Mat nv12_img(1224 * 3 / 2, 1632, CV_8UC1, buffer[buf.index].start);//可换rga
        // cvtColor(nv12_img, bgr_img, COLOR_YUV2BGR_NV12);

        // 重新入队
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("[V4L2 ERROR] QBUF failed");
            break; 
        }

        if(bgr_img.empty()) {
            cout << "[READQUEUE]: video read failed" << endl;
            break;
        }
        // imwrite("video.jpg",bgr_img);
        // 推入队列 (请确保 FramePacket 和 read_queue 是你在外部定义好的)
        FramePacket frame_pkt = {frame_id++, move(bgr_img)};
        read_queue.enqueue(move(frame_pkt));
        
        if(frame_id % 100 == 0)
            cout << "Read frame id: " << frame_id << endl;
        // if(frame_id > 900)
        //     break;
    }

    // ================= 6. 安全清理 =================
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    // for (int i = 0; i < req.count; ++i) {
    //     if (buffer[i].start != MAP_FAILED && buffer[i].start != nullptr) {
    //         munmap(buffer[i].start, buffer[i].length);
    //     }
    // }
    for (int i = 0; i < req.count; ++i) {
        if (buffer[i].exprot_fd > 0) {
            close(buffer[i].exprot_fd);
        }
    }
    close(fd);
    read_finish.store(true);
    cout << "Read thread finished" << endl;
}
static void USB_Read_func(int &fd)
{

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
                for(int i = 0; i < RING_BUFFER_SIZE; ++i)
                {
                    if(ring_buffer[i].valid())
                    {
                        all_tasks_done = false;
                        break;
                    }   
                        
                }
                
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
            {
                this_thread::sleep_for(chrono::milliseconds(1));
            }
        }

        size_t target_slot = next_frame_id % RING_BUFFER_SIZE;
        if(ring_buffer[target_slot].valid())
        {
            if(ring_buffer[target_slot].wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                ProcessData result = ring_buffer[target_slot].get();
                yolov5s::draw_result(result.processed_frame,result.results);
                
                FramePacket temp_pkt = {next_frame_id,move(result.processed_frame)};
                write_queue.enqueue(move(temp_pkt));
                next_frame_id++;
                if(next_frame_id % 10 == 0)
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
    FramePacket poison = {-1, cv::Mat(),Mat()};
    write_queue.enqueue(std::move(poison));
    cout<<"Process thread finished\n";
}
static void Write_func(cv::VideoWriter &writer)
{
    
    FramePacket frame_pkt;
    Mat temp_img;
    int frame_write_id = 0;
    int target_width = 1632;
    int target_height = 1224;
    MPP mpp;
    mpp.init(target_width,target_height,30);
    Mat nv12_img(1224 * 3 / 2, 1632,CV_8UC1);
    // FILE *outfp = fopen("output.h264","wb+");
    
    string rtmp_url = "rtmp://192.168.5.32/live/test"; // 把 IP 换成你刚才查到的
    // string ffmpeg_cmd = "ffmpeg -f h264 -framerate 30 -i pipe:0 -c:v copy -fflags nobuffer -flush_packets 1 -f flv " + rtmp_url;
    FILE *outfp = fopen("output.h264","wb+");
    zl_media streamer;
    if(!streamer.media_init(target_width,target_height,30))
    {
        cerr<<"zlmedia falied"<<endl;
        return;
    }
    if(!outfp)
    {
        cerr<<"error url"<<endl;
        return;
    }

    
    while (1)
    {
        write_queue.dequeue(frame_pkt);
        
        if(frame_pkt.frame_id == -1 )
        {
            void* p_data = nullptr; size_t p_size = 0; bool is_key = false;
            mpp.encoder(cv::Mat(),target_width,target_height,true,nullptr);
            break;
        }
                
        
        
        
        

        if(!frame_pkt.frame.empty())
        {
            
            temp_img = frame_pkt.frame;
            void *data = nullptr;
            size_t size = 0;
            bool iskey = false;
            
            
            
            rga_buffer_t src_rga = wrapbuffer_virtualaddr(temp_img.data,target_width,target_height,RK_FORMAT_BGR_888);
            rga_buffer_t dst_rga = wrapbuffer_virtualaddr(nv12_img.data,target_width,target_height,RK_FORMAT_YCbCr_420_SP);
            imcvtcolor(src_rga,dst_rga,RK_FORMAT_BGR_888,RK_FORMAT_YCbCr_420_SP);
            // writer.write(move(temp_img));
            mpp.encoder(nv12_img,target_width,target_height,false,
                [&streamer,outfp](void *data,size_t size,bool is_key){
                    streamer.push_frame(data,size);
                    if(outfp)
                        fwrite(data,1,size,outfp);
                    });
            
            //处理图像
            
            frame_write_id++;
            if(frame_write_id % 10 == 0)
                cout<<"Write frame id: "<<frame_write_id<<endl;
        }
        

    }
    // writer.release();
    pclose(outfp);
    write_finish.store(true);
    cout<<"Write thread finished\n";
    
}
int main() 
    {
        auto start = chrono::high_resolution_clock::now();
        
        int fd_mipi = open("/dev/video12",O_RDWR | O_NONBLOCK,0);
        if(fd_mipi < 0)
            cerr<<"打开mipi摄像头失败"<<endl;
        cout<<"打开mipi摄像头成功"<<endl;
        int fd_usb = open("video20",O_RDWR | O_NONBLOCK,0);
        if(fd_mipi < 0)
            cerr<<"打开usb摄像头失败"<<endl;
        cout<<"打开usb摄像头成功"<<endl;

        cv::VideoCapture cap(video_path);
        cv::Mat img;
        img = imread("/home/cat/linux_Ai/f4a59f21f5885fbf3192ff204247ca33.jpeg");
        
        if(!cap.isOpened())
        {
            cout<<"video open failed\n";
            return -1;
        }
        
        int video_fps = 30;
        int height = 1224; 
        int width = 1632;
        //H264
        cv::VideoWriter writer(video_output_path,cv::VideoWriter::fourcc('M','J','P','G'),video_fps,cv::Size(width,height));
        if (!writer.isOpened()) {
            std::cerr << "【错误】无法打开视频写入器！" << std::endl;
            std::cerr << "请检查：1.输出路径是否正确？ 2.是否有 'output' 文件夹？ 3.编码器是否支持？" << std::endl;
            return -1;
        }
        PostProcess post(label_path);
        ThreadPool thread_pool(model_path,3);
        // yolov5s yolo(model_path,3);
        // result_group r;
        // yolo.inference_img(img,r);
        // yolo.draw_result(img,r);
        // imwrite("outimg.jpg",img);
        // while(1);
        thread mipi_Read_T(Read_func,std::ref(fd_mipi));
        thread usb_read(USB_Read_func,ref(fd_usb));
        thread Process_T(Process_func,ref(thread_pool));
        thread Write_T(Write_func,ref(writer));

        mipi_Read_T.join();
        usb_read.join();
        Process_T.join();
        Write_T.join();

        cap.release();

        read_queue.stop();
        write_queue.stop();
        

        auto end = chrono::high_resolution_clock::now();
        auto duration_time = chrono::duration_cast<chrono::milliseconds>(end - start).count();
        cout<<"duration time cast "<<duration_time<<" ms"<<endl;
        cout << "真实物理帧率: " << (900.0 / (duration_time / 1000.0)) << " FPS" << endl;
        return 0;
}
