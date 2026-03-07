#ifndef __POST_PROCESS_H
#define __POST_PROCESS_H
#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include "yolov5s.h"
using namespace std;
#define CLASS_MAX_NUM 80    
#define BOX_THRESHOLD 0.5
#define NMS_THRESHOLD 0.45


struct box_t
{
    float xmin;
    float ymin;
    float xmax;
    float ymax;
};

struct detect_result_t
{
    char label[32];
    box_t box;
    float box_conf;
};
struct result_group
{
    int count;//框的数量
    detect_result_t result[64];

};
class PostProcess
{
public:
    PostProcess(const  string &label_path);
    ~PostProcess(){};
private:
    size_t load_model(const string &label_path, vector<string> &labels);

};
int post_process(int8_t *output0,int8_t *output1,int8_t *output2,int model_height,int model_width,
                  float nms_threshold,float box_threshold, float scale_w,float scale_h,
                vector<int32_t>& qnt_zps, std::vector<float>& qnt_scales,result_group &results
                );
#endif // __POST_PROCESS_H
