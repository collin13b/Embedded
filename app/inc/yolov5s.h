#ifndef __YOLOV5S_H
#define __YOLOV5S_H

#include <iostream>
#include <vector>
#include <ranges>
#include <fstream>
#include <string>

#include <opencv2/core.hpp>
#include <opencv4/opencv2/highgui.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include "rga.h"
#include "RgaUtils.h"
#include "im2d.h"
#include "rknn_api.h"
#include <chrono>
#include <iomanip>
#include <sstream>
using namespace std;
using namespace cv;
struct result_group;
class yolov5s
{
public:
    yolov5s(const string &mode_path, int npu_index);
    ~yolov5s();

    int model_height = 0;
    int model_width = 0;
    int model_channel = 0;

    int img_height = 0;
    int img_width = 0;
    int img_channel = 0 ;

    int inference_img(Mat &img,result_group &results);
    static int draw_result(Mat &img,result_group &results);
private:
    rknn_context ctx;
    size_t model_size;

    rknn_tensor_attr input_tensor;
    rknn_tensor_attr output_tensor;

    rknn_input_output_num io_num;

    vector<rknn_tensor_attr> input_tensors;
    vector<rknn_tensor_attr> output_tensors; 
    
    vector<uint8_t> model_data;
    vector<uint8_t> load_model(const string &model_path);

};
class Rgabuffer
{
public:
    Rgabuffer(size_t size,int height,int width,int format){
        ptr = (char *)malloc(size);
        handle = importbuffer_virtualaddr(ptr,size);
        if(handle == 0)
        {
            free(ptr);
            throw runtime_error("RGA buffer import failed");
        }
        rga_buf = wrapbuffer_handle(handle,width,height,format);
        this->size = size;
    };
    ~Rgabuffer(){
        if(handle) releasebuffer_handle(handle);
        if(ptr) free(ptr);
    };
    char *get_ptr() const { return ptr; }
    rga_buffer_t get_rga_buf() const { return rga_buf; }
    rga_buffer_handle_t get_handle() const { return handle; }
private:
    char *ptr = nullptr;
    rga_buffer_handle_t handle = 0;
    rga_buffer_t rga_buf;
    size_t size;
};
#endif
