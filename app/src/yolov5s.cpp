#include "yolov5s.h"
#include "post_process.h"

namespace fs = std::filesystem;
yolov5s::yolov5s(const string &model_path, int npu_index)
{
    try
    {
        this->model_data = load_model(model_path);
    }
    catch(const std::exception& e)
    {
        cerr << "Error loading model: " << e.what() << endl;
        throw; // Rethrow the exception after logging
    }

    int ret = rknn_init(&this->ctx,this->model_data.data(),model_data.size(),RKNN_FLAG_PRIOR_HIGH,NULL);
    if(ret  != 0)
        printf("rknn init faild %d \n",ret);
    else
        printf("rknn init success! \n");
    rknn_core_mask core_mask;
    switch (npu_index)
    {
    case 0:core_mask = RKNN_NPU_CORE_0; break;
    case 1:core_mask = RKNN_NPU_CORE_1; break;
    case 2:core_mask = RKNN_NPU_CORE_2; break;
    default:core_mask = RKNN_NPU_CORE_AUTO; break; 
    }
    ret = rknn_set_core_mask(ctx,core_mask);
    if(ret != 0)
        std::cerr<<"set npu core mask failed "<<ret<<std::endl;

    ret = rknn_query(ctx,RKNN_QUERY_IN_OUT_NUM,&io_num,sizeof(io_num));
    if(ret != 0)
        std::cerr<<"rknn query io_num failed "<<ret<<std::endl;

    input_tensors.resize(io_num.n_input);
    output_tensors.resize(io_num.n_output);

    for(uint32_t i = 0; i < io_num.n_input;i++)
    {
        input_tensors[i].index = i;
        rknn_query(ctx,RKNN_QUERY_INPUT_ATTR,&input_tensors[i],sizeof(rknn_tensor_attr));
        printf("input tensor %d: name=%s, dims=[%d,%d,%d], n_elems=%d, size=%d\n",i,input_tensors[i].name,
            input_tensors[i].dims[1],input_tensors[i].dims[2],input_tensors[i].dims[3],
            input_tensors[i].n_elems,input_tensors[i].size);
    }
    for(uint32_t i = 0; i < io_num.n_output;i++)
    {
        output_tensors[i].index = i;
        rknn_query(ctx,RKNN_QUERY_OUTPUT_ATTR,&output_tensors[i],sizeof(rknn_tensor_attr));
        printf("output tensor %d: name=%s, dims=[%d,%d,%d], n_elems=%d, size=%d\n",i,output_tensors[i].name,
            output_tensors[i].dims[1],output_tensors[i].dims[2],output_tensors[i].dims[3],
            output_tensors[i].n_elems,output_tensors[i].size);
    }
    if(input_tensors[0].fmt == RKNN_TENSOR_NCHW)
    {
        model_channel = input_tensors[0].dims[1];
        model_height = input_tensors[0].dims[2];
        model_width = input_tensors[0].dims[3];
        cout<<"model input format is NCHW\n";
    }
    else if(input_tensors[0].fmt == RKNN_TENSOR_NHWC)
    {
        model_height = input_tensors[0].dims[1];
        model_width = input_tensors[0].dims[2];
        model_channel = input_tensors[0].dims[3];
        cout<<"model input format is NHWC\n";
    }
    std::cout << "init yolo successful" << std::endl;
    printf("model input height=%d, width=%d, channel=%d\n",model_height,model_width,model_channel);
}
yolov5s::~yolov5s()
{
    if(ctx)
    {
        rknn_destroy(ctx);
        ctx = 0;
    }
}
vector<uint8_t> yolov5s::load_model(const string &model_path)
{
    ifstream file(model_path,ios::binary | ios::ate);

    if(!file.is_open())
    {
        throw runtime_error("model file open failed");
    }
    std:: streamsize size = file.tellg();
    file.seekg(0,ios::beg);

    vector<uint8_t> data(size);

    if(!file.read(reinterpret_cast<char *>(data.data()),size))
    {
        throw runtime_error("model data read failed");
    }
    return data;
}
int yolov5s::inference_img(Mat &img,result_group &results)
{
    this->img_height = img.rows;
    this->img_width = img.cols;
    this->img_channel = img.channels();

    Mat bkg;

    if(this->img_height % 16 != 0 || this->img_width % 16 != 0 )
    {
        int bkg_h = (this->img_height + 15) / 16 * 16;
        int bkg_w = (this->img_width + 15) / 16 * 16;
        cv::copyMakeBorder(img,bkg,0,bkg_h - this->img_height,0,bkg_w - this->img_width,cv::BORDER_CONSTANT,cv::Scalar(114,114,114));
        this->img_width = bkg_w;
        this->img_height = bkg_h;
    }
    else
    {
        bkg = img;
    }
    auto start_time = std::chrono::high_resolution_clock::now();

    // printf("preprocess start\n");
    try{
        size_t src_size = img_width * img_height * 3;
        size_t dst_size = model_width * model_height * model_channel;
        Rgabuffer src_buf(src_size,img_height,img_width, RK_FORMAT_BGR_888);
        Rgabuffer cvt_buf(src_size,img_height,img_width, RK_FORMAT_RGB_888);
        Rgabuffer dst_buf(dst_size,model_height,model_width, RK_FORMAT_RGB_888);

        rga_buffer_handle_t src_handle = src_buf.get_handle();
        rga_buffer_handle_t cvt_handle = cvt_buf.get_handle();
        rga_buffer_handle_t dst_handle = dst_buf.get_handle();

        rga_buffer_t src_rga_buf = src_buf.get_rga_buf();
        rga_buffer_t cvt_rga_buf = cvt_buf.get_rga_buf();
        rga_buffer_t dst_rga_buf = dst_buf.get_rga_buf();

        // Copy the padded image data to src_buf
        memcpy(src_buf.get_ptr(), bkg.data, src_size);

        int ret = imcvtcolor(src_rga_buf,cvt_rga_buf,RK_FORMAT_BGR_888,RK_FORMAT_RGB_888);
        if(ret != IM_STATUS_SUCCESS) throw runtime_error("imcvtcolor failed");
        ret = imresize(cvt_rga_buf,dst_rga_buf);
        if(ret !=IM_STATUS_SUCCESS) throw runtime_error("imresize failed");
        //设置输入
        vector<rknn_input> inputs(io_num.n_input);
        memset(inputs.data(),0,sizeof(rknn_input)*inputs.size());

        inputs[0].index = 0;
        inputs[0].buf = dst_buf.get_ptr();
        inputs[0].size = dst_size;
        inputs[0].pass_through = false;
        inputs[0].type = RKNN_TENSOR_UINT8;
        inputs[0].fmt = RKNN_TENSOR_NHWC;

        rknn_inputs_set(ctx,io_num.n_input,inputs.data());
        int outputs_num = io_num.n_output;
        vector<rknn_output> outputs(io_num.n_output);
        for(int i = 0; i < outputs_num; ++i)
        {
            outputs[i].want_float = 0;
        }
        //推理
        ret = rknn_run(ctx,nullptr);
        if(ret != 0) throw runtime_error("rknn_run failed");
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);  
        // cout<<"preprocess time "<<duration_time.count()<<" ms"<<endl;
        
        ret = rknn_outputs_get(ctx,io_num.n_output,outputs.data(),nullptr);
        if(ret != 0) throw runtime_error("rknn_outputs_get failed");



        float scale_w = (float)model_width / img_width;
        float scale_h = (float)model_height / img_height;
        
        vector<int32_t> qnt_zps;
        vector<float> qnt_scales;
        qnt_zps.reserve(200);
        qnt_scales.reserve(200);
        for(int i = 0; i < io_num.n_output; ++i)
        {
            qnt_zps.emplace_back(output_tensors[i].zp);
            qnt_scales.emplace_back(output_tensors[i].scale);
        }
        post_process((int8_t *)outputs[0].buf,(int8_t *)outputs[1].buf,(int8_t *)outputs[2].buf,model_height,model_width,0.5,0.5,scale_w,scale_h,qnt_zps,qnt_scales,results);

    }
        catch(const std::exception& e)
        {            cerr << "Error during preprocessing: " << e.what() << endl;
            return -1; // Return an error code or handle it as needed
        }

    return 0;
}
static cv::Scalar get_color(const string &label)
{
    hash<string> hasher;
    size_t hash = hasher(label);

    int r = (hash & 0xFF0000) >> 16;
    int g = (hash & 0x00FF00) >> 8;
    int b = (hash & 0x0000FF);
    return cv::Scalar(b,g,r);
}
int yolov5s::draw_result(Mat &img,result_group &results)
{
    
    for(int i = 0; i <results.count; ++i)
    {
        int xmin = results.result[i].box.xmin;
        int ymin = results.result[i].box.ymin;
        int xmax = results.result[i].box.xmax;
        int ymax = results.result[i].box.ymax;

        float conf = results.result[i].box_conf;
        string label_name = results.result[i].label;
        cv::Scalar color = get_color(label_name);

        //huakuang
        rectangle(img,cv::Point(xmin,ymin),cv::Point(xmax,ymax),color,2);

        char text[256];
        snprintf(text,sizeof(text),"%s %.1f%%",label_name.c_str(),conf * 100);

        int baseline = 0;
        double font_scale = 1.5;
        int thinckness = 2;
        Size label_size = getTextSize(text,FONT_HERSHEY_SIMPLEX,font_scale,thinckness,&baseline);
        
        int label_min = max(ymin,label_size.height + 5);

        cv::rectangle(img,
                      Point(xmin,label_min - label_size.height - 5),
                      Point(xmin + label_size.width,label_min + baseline),
                      color,
                      cv::FILLED
                    );

        cv::putText(img,
                    text,
                    Point(xmin,label_min),
                    FONT_HERSHEY_SIMPLEX,font_scale,
                    cv::Scalar(0,255,0),thinckness
        );
    }
    return 0;
}
