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
static size_t MAX_CONCURRENT_FRAMES = 16;
struct FramePacket
{
    int streamer_id;
    int frame_id;
    cv::Mat frame;
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
static mutex usb_mutex;
static Mat latest_usb_img;

static SafeQueue<FramePacket> read_queue(3);
static SafeQueue<FramePacket> read_usb_queue(3);
static SafeQueue<FramePacket> write_queue(3);
static SafeQueue<FramePacket> write_usb_queue(3);
atomic<bool> read_finish{false};
atomic<bool> usb_read_finish{false};

atomic<bool> process_finish{false};

atomic<bool> write_finish{false};  

static vector<cv::Mat> bgr_pool(100);
static bool pool_initialized = false;
static std::mutex rga_mtx;
static void bgr_to_nv12_cpu(const cv::Mat& bgr, cv::Mat& nv12, int width, int height) {
    cv::Mat i420;
    // 1. 利用 OpenCV 多线程软转 I420
    cv::cvtColor(bgr, i420, cv::COLOR_BGR2YUV_I420);
    
    int y_size = width * height;
    int uv_size = y_size / 4;
    
    // 2. 拷贝 Y 平面
    memcpy(nv12.data, i420.data, y_size);
    
    // 3. 将 I420 的 U 和 V 交织合并成 NV12 格式
    uint8_t* u_ptr = i420.data + y_size;
    uint8_t* v_ptr = u_ptr + uv_size;
    uint8_t* uv_dst = nv12.data + y_size;
    
    for (int i = 0; i < uv_size; ++i) {
        uv_dst[i * 2] = u_ptr[i];
        uv_dst[i * 2 + 1] = v_ptr[i];
    }
}
static void nv12_to_bgr(Mat &img,Mat &cvt_img,int index,vector<Buffer> &buffer)
{
    int img_width = img.cols;
    int img_height = img.rows;
    
    // RGA 官方宏会自动处理像素步长，不要自己算！
    rga_buffer_t src_rga = wrapbuffer_virtualaddr(buffer[index].start, img_width, img_height, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst_rga = wrapbuffer_virtualaddr(cvt_img.data, img_width, img_height, RK_FORMAT_BGR_888);
    
    int rga_ret;
    {
        lock_guard<mutex> lock(rga_mtx);
        rga_ret = imcvtcolor(src_rga, dst_rga, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888);
    }
    
    if(rga_ret != IM_STATUS_SUCCESS) {
        cerr << "[rga error]"<<endl;
    }
}
static void nv12_to_bgr_dma(int img_width, int img_height, Mat &cvt_img, int fd, vector<Buffer_dma> &buffer)
{
    if (cvt_img.empty() || cvt_img.cols != img_width || cvt_img.rows != img_height) {
        cvt_img.create(img_height, img_width, CV_8UC3); 
    }

    // 算出 16 字节对齐的物理地基 (1232)
    int rga_align_h = (img_height + 15) / 16 * 16;

    // 1. 正常传入 1224 的真实高度
    rga_buffer_t src_rga = wrapbuffer_fd(fd, img_width, img_height, RK_FORMAT_YCbCr_420_SP);
    
    // 2. 【极其关键的魔法】：强行覆盖物理地基为 1232！
    src_rga.hstride = rga_align_h; 
    
    // 3. 目标是 BGR，不需要改地基
    rga_buffer_t dst_rga = wrapbuffer_virtualaddr(cvt_img.data, img_width, img_height, RK_FORMAT_BGR_888);
    
    int rga_ret;
    {
        lock_guard<mutex> lock(rga_mtx);
        rga_ret = imcvtcolor(src_rga, dst_rga, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888);
    } 
    if(rga_ret != IM_STATUS_SUCCESS) cerr << "[Read RGA error]"<<endl;
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
    req.count = 10;
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
            if (errno == EAGAIN) continue; // 没准备好就等，绝对不能 break！
            perror("[V4L2 ERROR] DQBUF failed");
            break;
        }

        // 处理图像
        // Mat nv12_img(1224 * 3 / 2,1632,CV_8UC3);
        // memcpy(nv12_img.data,buffer[buf.index].start,buffer[buf.index].length);

        Mat &bgr_img = bgr_pool[frame_id % 128];
        int cur_fd = buffer[buf.index].exprot_fd;
        nv12_to_bgr_dma(1632,1224,bgr_img,cur_fd,buffer);
        // nv12_to_bgr(bgr_img,bgr_img,buf.index,buffer);
        // Mat nv12_img(1224 * 3 / 2, 1632, CV_8UC1, buffer[buf.index].start);//可换rga
        // cvtColor(nv12_img, bgr_img, COLOR_YUV2BGR_NV12);

        // 重新入队
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { // 必须是 QBUF ！！！
            perror("[V4L2 ERROR] QBUF failed");
            break; 
        }

        if(bgr_img.empty()) {
            cout << "[READQUEUE]: video read failed" << endl;
            break;
        }
        // imwrite("video.jpg",bgr_img);
        // 推入队列 (请确保 FramePacket 和 read_queue 是你在外部定义好的)
        FramePacket frame_pkt = {0,frame_id++, (bgr_img)};
        read_queue.enqueue(move(frame_pkt));
        
        if(frame_id % 100 == 0)
            cout << "Read frame id: " << frame_id << endl;
        // if(frame_id > 300)
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
    int frame_id = 0;
    int ret;
    struct v4l2_format fmt;
    memset(&fmt,0,sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = 1280;
    fmt.fmt.pix.height = 720;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field  = V4L2_FIELD_ANY;
    if(ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("[USB V4L2] set fmt failed");
        usb_read_finish.store(true);
        return;
    }

    // 2. 申请缓冲区
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 10;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    
    if(ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("[USB V4L2] reqbufs failed");
        usb_read_finish.store(true);
        return;
    }

    // 3. 内存映射 (注意：单平面的映射方法比 MPLANE 简单得多)
    vector<Buffer> buffer(req.count);
    for(int i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        ioctl(fd, VIDIOC_QUERYBUF, &buf);
        buffer[i].length = buf.length;
        buffer[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        
        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    // 4. 开启数据流
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMON, &type);

    struct pollfd pool_fds[1];
    pool_fds[0].fd = fd;
    pool_fds[0].events = POLLIN;

    int physical_frame_count = 0;
    // 5. 抓图循环
    while(!usb_read_finish.load()) {
        ret = poll(pool_fds, 1, 5000);
        if(ret <= 0) continue;

        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if(ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue; // 没准备好就等，绝对不能 break！
            break;
        }

        // 【工程核心】将 MJPEG 内存流直接解码为 BGR Mat
        // 这里没有使用 RGA，因为 RK 芯片的 RGA 通常不支持 JPEG 硬件解码，用 OpenCV 软解最稳妥
        
        physical_frame_count++;
        if (physical_frame_count % 3 != 0) {
            ioctl(fd, VIDIOC_QBUF, &buf); // 立刻还回缓冲区，避免掉帧
            continue; // 丢弃 2/3 的画面，直接去抓下一帧
        }
        cv::Mat raw_data(1, buf.bytesused, CV_8UC1, buffer[buf.index].start);
        cv::Mat bgr_img = cv::imdecode(raw_data, cv::IMREAD_COLOR);

        ioctl(fd, VIDIOC_QBUF, &buf); // 立刻还回缓冲区，避免掉帧
        if(!bgr_img.empty()) {
            FramePacket frame_pkt = {1,frame_id++,move(bgr_img)};
            read_usb_queue.enqueue(move(frame_pkt));

        }
        
        // if(frame_id > 300) break; // 同样限制 300 帧测试
    }

    // 6. 安全清理
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for(int i = 0; i < req.count; ++i) {
        munmap(buffer[i].start, buffer[i].length);
    }
    close(fd);
    usb_read_finish.store(true);
    cout << "USB Read thread finished" << endl;
}
static void Process_func(ThreadPool &thread_pool,SafeQueue<FramePacket>&in_queue,SafeQueue<FramePacket>& out_queue,atomic<bool> &in_finsh)
{
    auto start = chrono::high_resolution_clock::now();
    int next_frame_id = 0;
    FramePacket pending_pkt;
    bool has_pending = false;

    size_t RING_BUFFER_SIZE = MAX_CONCURRENT_FRAMES * 2;
    std::vector<std::future<ProcessData>> ring_buffer(RING_BUFFER_SIZE);
    while(true)
    {
        if(!has_pending)
        {
            if(!in_queue.isEmpty())
            {
                if(in_queue.dequeue(pending_pkt))
                {
                    has_pending = true;
                }
            }
            else if(in_finsh.load())
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
                
                FramePacket temp_pkt = {pending_pkt.streamer_id,next_frame_id,result.processed_frame};
                out_queue.enqueue(move(temp_pkt));
                
                next_frame_id++;
                if(next_frame_id % 100 == 0)
                    cout<<"Process frame id: "<<next_frame_id<<endl;
            }
        }
        else
        {
            if(in_queue.isEmpty())
                this_thread::sleep_for(chrono::milliseconds(1));
            else if(has_pending)
                this_thread::sleep_for(chrono::milliseconds(1));
        }
    }
    process_finish.store(true);
    FramePacket poison = {-1,-1, cv::Mat()};
    out_queue.enqueue(std::move(poison));
    cout<<"Process thread finished\n";
}

static void Write_func(int streamer_id,SafeQueue<FramePacket>&queue,int target_w,int target_h, const string &streame_name,int fps)
{
    
    FramePacket frame_pkt;
    Mat temp_img;
    int frame_write_id = 0;
    // int target_width = 1632;
    // int target_height = 1224;
    MPP mpp;
    mpp.init(target_w,target_h,fps);
    int rga_align_h = (target_h + 15) /16 * 16;
    Mat nv12_img(rga_align_h * 3 / 2, target_w,CV_8UC1);
    // FILE *outfp = fopen("output.h264","wb+");
    string filename = "output_" + streame_name + ".h264";
    FILE *outfp = fopen(filename.c_str(),"wb+");
    zl_media streamer;
    if(!streamer.media_init(target_w,target_h,fps,streame_name))
    {
        cerr<<"zlmedia falied"<<endl;
        return;
    }
    if(!outfp)
    {
        cerr<<"error url"<<endl;
        return;
    }
    uint32_t frame_pts = 0;
    auto start_time_point = std::chrono::steady_clock::now();
    while (1)
    {
        queue.dequeue(frame_pkt);
        
        if(frame_pkt.frame_id == -1 )
        {
            void* p_data = nullptr; size_t p_size = 0; bool is_key = false;
            mpp.encoder(cv::Mat(),target_w,target_h,true,nullptr);
            break;
        }
        if(!frame_pkt.frame.empty())
        {
            auto now = std::chrono::steady_clock::now();
            temp_img = frame_pkt.frame;
            void *data = nullptr;
            size_t size = 0;
            bool iskey = false;
            // 
            uint32_t current_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_point).count();
            
            
            if(streamer_id == 1)
            {
                bgr_to_nv12_cpu(temp_img, nv12_img, target_w, target_h);
            }
            else
            {
                rga_buffer_t src_rga = wrapbuffer_virtualaddr(temp_img.data, target_w, target_h, RK_FORMAT_BGR_888);
                rga_buffer_t dst_rga = wrapbuffer_virtualaddr(nv12_img.data, target_w, target_h, RK_FORMAT_YCbCr_420_SP);
                dst_rga.hstride = rga_align_h;
                {
                lock_guard<mutex> lock(rga_mtx);
                int ret = imcvtcolor(src_rga, dst_rga, RK_FORMAT_BGR_888, RK_FORMAT_YCbCr_420_SP);
                if(ret != IM_STATUS_SUCCESS) {
                    cerr << "[Write RGA error] ret=" << ret << endl;
                }
                }
            }
            
            // dst_rga.hstride = rga_align_h;
            
            
            // writer.write(move(temp_img));
            
            mpp.encoder(nv12_img,target_w,target_h,false,
                [&streamer,outfp,current_time_ms](void *data,size_t size,bool is_key){
                   
                    streamer.push_frame(data,size,current_time_ms);
                    
                    // if(outfp)
                    //     fwrite(data,1,size,outfp);
                    });
            
            //处理图像
            
            frame_write_id++;
            if(frame_write_id % 100 == 0)
                cout<<"Write frame id: "<<frame_write_id<<endl;
        }
        

    }
    // writer.release();
    // auto end = chrono::high_resolution_clock::now();
    //                 auto duration_time = chrono::duration_cast<chrono::milliseconds>(end - start).count();
    //                 cout << "cost"<<duration_time<<"ms"<<endl;
    // fclose(outfp);
    write_finish.store(true);
    cout<<"Write thread finished\n";
    
}
static void write_func()
{

}
int main() 
    {
        auto start = chrono::high_resolution_clock::now();
        if(!zl_media::global_init())
        {
                cerr<<"init zlmidia failed"<<endl;
                return -1;
        }
        if (!pool_initialized) {
        for (int i = 0; i < 128; i++) {
            bgr_pool.emplace_back(cv::Mat(1224, 1632, CV_8UC3)); 
        }
        pool_initialized = true;
        }

        int fd_mipi = open("/dev/video12",O_RDWR | O_NONBLOCK,0);
        if(fd_mipi < 0)
            cerr<<"打开mipi摄像头失败"<<endl;
        cout<<"打开mipi摄像头成功"<<endl;
        int fd_usb = open("/dev/video20",O_RDWR | O_NONBLOCK,0);
        if(fd_usb < 0)
            cerr<<"打开usb摄像头失败"<<endl;
        cout<<"打开usb摄像头成功"<<endl;

        // cv::VideoCapture cap(video_path);
        // cv::Mat img;
        // img = imread("/home/cat/linux_Ai/f4a59f21f5885fbf3192ff204247ca33.jpeg");
        
        // if(!cap.isOpened())
        // {
        //     cout<<"video open failed\n";
        //     return -1;
        // }
        
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
        vector<int> id = {1,2};
        vector<int> id1 = {0};
        ThreadPool thread_pool_mipi(model_path,2,id);
        ThreadPool thread_pool_usb(model_path,1,id1);

        thread mipi_Read_T(Read_func,std::ref(fd_mipi));
        thread usb_read(USB_Read_func,ref(fd_usb));

        thread Process_T(Process_func,ref(thread_pool_mipi),ref(read_queue),ref(write_queue),ref(read_finish));
        thread Process_usb_T(Process_func,ref(thread_pool_usb),ref(read_usb_queue),ref(write_usb_queue),ref(usb_read_finish));

        thread Write_T(Write_func,0,ref(write_queue),1632,1224,"mipi",30);
        thread write_usb_T(Write_func,1,ref(write_usb_queue),1280,720,"usb",10);

        mipi_Read_T.join();
        usb_read.join();
        Process_T.join();
        Process_usb_T.join();
        Write_T.join();
        write_usb_T.join();

        // cap.release();

        read_queue.stop();
        write_queue.stop();
        

        auto end = chrono::high_resolution_clock::now();
        auto duration_time = chrono::duration_cast<chrono::milliseconds>(end - start).count();
        cout<<"duration time cast "<<duration_time<<" ms"<<endl;
        cout << "真实物理帧率: " << (300.0 / (duration_time / 1000.0)) << " FPS" << endl;
        return 0;
}
