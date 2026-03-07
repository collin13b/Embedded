#include "post_process.h"
#include <stdio.h>
#include <numeric> // 用于 std::iota
#include <algorithm>
float anchor0[6] = {10, 13, 16, 30, 33, 23};
float anchor1[6] = {30, 61, 62, 45, 59, 119};
float anchor2[6] = {116, 90, 156, 198, 373, 326};
vector<string> labels;
struct Prob_box
{
    float conf;
    int index;
};

PostProcess::PostProcess(const string &label_path)
{
    size_t labels_size = load_model(label_path, labels);
    cout<< "Loaded "<<labels_size<<" labels from "<<label_path<<endl;
    for(auto &label : labels)
    {
        cout<<"Loaded label: "<<label<<endl;
    }
}

size_t PostProcess::load_model(const string &label_path, vector<string> &labels)
{
    ifstream label_file(label_path);
    if(!label_file.is_open())
    {
        cerr<<"Failed to open label file: "<<label_path<<endl;
        return 0;
    }
    string line;
    while(getline(label_file, line))
    {
        labels.emplace_back(line);
        if(labels.size() >= 80) // yolov5s has 80 classes
            break;
    }
    return labels.size();
}
//sigmoid函数和反sigmoid函数
static float sigmoid(float x)
{
    float y = 1.0f / (1.0f + exp(-x));
    return y;
}
static float unsigmoid(float y)
{
    float x = -1.0f * log(1.0f / y - 1.0f);
    return x;
}
//限制函数，将输入值限制在[min, max]范围内，并返回一个整数
inline static int32_t __limit_num(float val, float min, float max)
{
    float f = val <= min ? min : (val >= max ? max : val);
    return static_cast<int32_t>(f);
}

//量化和反量化函数
static int8_t qnt_f32_to_int8(float val,int32_t zp,float scale)
{
    float float_val = (val / scale) + zp;
    int8_t qnt_val = static_cast<int8_t>(__limit_num(float_val, -128.0f, 127.0f));
    return qnt_val;
}

static float deqnt_int8_to_f32(int8_t val, int32_t zp, float scale)
{
    float deqnt_val = (static_cast<float>(val - zp)) * scale;
    return deqnt_val;
}

static int sort_prob_box(vector<Prob_box> &p_boxs)
{
    sort(p_boxs.begin(),p_boxs.end(),
    [](const Prob_box &aa, const Prob_box &b)
    {
        return aa.conf > b.conf;
    });
    return 0;    
}

static float box_table[256];
static bool box_table_inited = false;

void init_box_table(int32_t zp, float scale)
{
    if(box_table_inited)
        return;
    for(int i = 0; i < 256; ++i)
    {
        int8_t val = static_cast<int8_t>(i);
        float deq = deqnt_int8_to_f32(val, zp, scale);
        box_table[i] = sigmoid(deq);

    }
    box_table_inited = true;
}

inline float fast_sigmoid_deq(int8_t val) {
    // 将 int8 映射到 0-255 的数组下标
    return box_table[static_cast<uint8_t>(val)];
}

static float IOU(float xmin0, float ymin0, float xmax0, float ymax0,
                          float xmin1, float ymin1, float xmax1, float ymax1)
{
    // 计算两个矩形框的交集宽度和高度
    float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0);
    float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0);
    // 计算交集面积
    float i = w * h;
    // 计算并集面积
    float u = (xmax0 - xmin0 + 1.0) * (ymax0 - ymin0 + 1.0) + (xmax1 - xmin1 + 1.0) * (ymax1 - ymin1 + 1.0) - i;
    // 计算交并比，如果并集面积为 0，返回 0，否则返回交集面积除以并集面积
    float iou = u <= 0.f? 0.f : (i / u);
    return iou;
}

static void nms(int valid_count,vector<float>&box,vector<int> &classid,
                vector<int> &indexArray,int cur_class,float nms_threshold)
{
    for(int i = 0; i < valid_count; ++i)
    {
        int n = indexArray[i];

        if(n == -1 || classid[n] != cur_class)
            continue;
        
        for(int j = i + 1; j < valid_count; ++j)
        {
            int m = indexArray[j];

            if(m == -1 || classid[m] != cur_class )
                continue;
            
            float xmin0 = box[n * 4];
            float ymin0 = box[n * 4 + 1];
            float xmax0 = box[n * 4 + 2] + xmin0;
            float ymax0 = box[n * 4 + 3] + ymin0;

            float xmin1 = box[m * 4];
            float ymin1 = box[m * 4 + 1];
            float xmax1 = box[m * 4 + 2] + xmin1;
            float ymax1 = box[m * 4 + 3] + ymin1;
            
            float iou = IOU(xmin0,ymin0,xmax0,ymax0,xmin1,ymin1,xmax1,ymax1);

            if(iou > nms_threshold)
                indexArray[j] = -1;
        }
    }
}
static int process(int8_t *input,float *anchor,int grid_h,int grid_w,int model_height,
                   int model_width,int stride,vector<float> &boxes,vector<float> &objProbs,vector<int> &classid,float box_threshold,int32_t zp,float scale   )
{
    if(!box_table_inited)
        init_box_table(zp, scale);
    float local_box_table[256];
    for(int i = 0; i < 256; ++i)
    {
        int8_t val = static_cast<int8_t>(i);
        float deq = deqnt_int8_to_f32(val, zp, scale);
        local_box_table[i] = sigmoid(deq);
    }
    int valid_num = 0;
    uint64_t grid_len = grid_h * grid_w;
    
    //反量化得到box_conf 置信框阈值，筛选置信框
    int8_t box_conf = qnt_f32_to_int8(unsigmoid(box_threshold),zp,scale);
    for(size_t a = 0; a < 3; ++a)
    {
        uint64_t offset_base = (a * 85) * grid_len;

        int8_t *p_x = input + offset_base;//x
        int8_t *p_y = input + offset_base + grid_len;//y
        int8_t *p_w = input + offset_base + 2 * grid_len;//w
        int8_t *p_h = input + offset_base + 3 * grid_len;//h
        int8_t *p_conf = input + offset_base + 4 * grid_len;
        int8_t *p_cls = input + offset_base + 5 * grid_len;
        for(size_t i = 0; i < grid_h; ++i)
        {
            for(size_t j = 0; j < grid_w; ++j)
            {
                int8_t cur_conf = *p_conf;
                if(cur_conf > box_conf)
                {
                    valid_num++;
                    
                    float x_sigmod = local_box_table[static_cast<uint8_t>(*p_x)];
                    float y_sigmod = local_box_table[static_cast<uint8_t>(*p_y)];
                    float w_sigmod = local_box_table[static_cast<uint8_t>(*p_w)];
                    float h_sigmod = local_box_table[static_cast<uint8_t>(*p_h)];

                    float w_temp = (w_sigmod * 2.0f);
                    float h_temp = (h_sigmod * 2.0f);
                    //计算出box的坐标和宽高
                    float box_x = (x_sigmod * 2.0f - 0.5f + j) * stride;
                    float box_y = (y_sigmod * 2.0f - 0.5f + i) * stride;
                    float box_w = w_temp * w_temp * anchor[a * 2];
                    float box_h = h_temp * h_temp * anchor[a * 2 + 1];

                    box_x = box_x - box_w * 0.5f;
                    box_y = box_y - box_h * 0.5f;

                    boxes.emplace_back(box_x);
                    boxes.emplace_back(box_y);
                    boxes.emplace_back(box_w);
                    boxes.emplace_back(box_h);
                    int8_t max_prob = -128;
                    int max_class_id = -1;
                    int8_t *p_cur_cls = p_cls;

                    for(size_t k = 0; k < 80; ++k)
                    {
                        int8_t cur_prob = *p_cur_cls;
                        if(cur_prob > max_prob)
                        {
                            max_prob = cur_prob;
                            max_class_id = k;
                        }
                        p_cur_cls += grid_len;
                    }
                    objProbs.emplace_back(sigmoid(deqnt_int8_to_f32(max_prob, zp, scale)));
                    classid.emplace_back(max_class_id);
                }
                p_x++;
                p_y++;
                p_w++;
                p_h++;
                p_conf++;
                p_cls++;
            }
        }
    }
    return valid_num;
}

inline static int clamp(float val,int min,int max)
{
    return val < min ? min : (val > max ? max : val); 
}

int post_process(int8_t *output0,int8_t *output1,int8_t *output2,int model_height,int model_width,
                  float nms_threshold,float box_threshold, float scale_w,float scale_h,
                vector<int32_t>& qnt_zps, std::vector<float>& qnt_scales,result_group &results
                )
{
    int valid_num;

    vector<float> box;
    vector<float> objProb;
    vector<int> classid;
    box.reserve(300 * 4);
    objProb.reserve(300);
    classid.reserve(300);
    //处理第一个输出
    int stride[] = {8,16,32};
    int grid_h0 = model_height / stride[0];
    int grid_w0 = model_width / stride[0];
    int valid_num0 = process(output0,anchor0,grid_h0,grid_w0,model_height,model_width,
                             stride[0],box,objProb,classid,BOX_THRESHOLD,qnt_zps[0],qnt_scales[0]);

    int grid_h1 = model_height / stride[1];
    int grid_w1 = model_width / stride[1];
    int valid_num1 = process(output1,anchor1,grid_h1,grid_w1,model_height,model_width,
                             stride[1],box,objProb,classid,BOX_THRESHOLD,qnt_zps[1],qnt_scales[1]);

    
    int grid_h2 = model_height / stride[2];
    int grid_w2 = model_width / stride[2];
    int valid_num2 = process(output2,anchor2,grid_h2,grid_w2,model_height,model_width,
                             stride[2],box,objProb,classid,BOX_THRESHOLD,qnt_zps[2],qnt_scales[2]);


    valid_num = valid_num0 + valid_num1 + valid_num2;
    if(valid_num == 0)
        return 0;
    // printf("validcount %d\n",valid_num);
    
    //极速间接排序
    vector<int> indexArray(valid_num);
    iota(indexArray.begin(),indexArray.end(),0);

    sort(indexArray.begin(),indexArray.end(),[&](int a,int b){
        return objProb[a] > objProb[b];
    });
    //找出检测到的类别并执行 nms
    bool seen_classes[80] = {false};
    for(int i = 0; i < valid_num; ++i)
    {
        seen_classes[classid[i]] = true;
    }

    for(int i = 0; i < 80; ++i)
    {
        if(seen_classes[i])
            nms(valid_num,box,classid,indexArray,i,NMS_THRESHOLD);
    }

    int count = 0;

    for(int i = 0; i < valid_num; ++i)
    {
        if(indexArray[i] == -1 || count >= 60)
            continue;
        
        int n = indexArray[i];

        float xmin = box[4 * n];
        float ymin = box[4 * n + 1];
        float xmax = box[4 * n + 2] + xmin;
        float ymax = box[4 * n + 3] + ymin;
        float box_conf = objProb[n];
        int id = classid[n];

        results.result[count].box_conf = box_conf;
        results.result[count].box.xmin = (int)(clamp(xmin,0,model_width) / scale_w);
        results.result[count].box.xmax = (int)(clamp(xmax,0,model_width) / scale_w);
        results.result[count].box.ymin = (int)(clamp(ymin,0,model_height) / scale_h);
        results.result[count].box.ymax = (int)(clamp(ymax,0,model_height) / scale_h);
        if (id < 0 || id >= labels.size()) {
        // printf("【致命错误】id 越界了！id = %d, labels.size() = %zu\n", id, labels.size());
        
        continue; // 跳过这个错误框
        }
        if (count >= 64) {
        std::cerr << "警告: 检测框数量超过最大限制 " << 64 << "，停止记录。" << std::endl;
        break; 
        }
        strncpy(results.result[count].label,labels[id].c_str(),31);
        results.result[count].label[31] = '\0';

        count++;
    }
    results.count = count;

    return 0;
}

