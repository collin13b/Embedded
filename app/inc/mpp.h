#ifndef _MPP_H
#define _MPP_H
#include <functional>
#include <iostream>
#include <rockchip/mpp_meta.h>
#include <rockchip/rk_mpi.h>
#include <opencv4/opencv2/core.hpp>
#include <opencv4/opencv2/highgui.hpp>
#ifndef MPP_PACKET_FLAG_INTRA
#define MPP_PACKET_FLAG_INTRA (0x00000008)
#endif

using namespace std;
class MPP{
public:  
    MPP(){};
        
    MppCtx ctx; 
    MppApi *mpi; 
    MppEncCfg cfg;

    MppFrame frame;
    MppPacket packet;
    MppBuffer buffer;

    bool init(int width,int height,int fps);
    // bool encoder(const cv::Mat nv12_img,FILE *out_fp,int width, int height,bool is_eos);
    bool encoder(const cv::Mat& nv12_img, int width, int height, bool is_eos, 
                 std::function<void(void* ptr, size_t len, bool is_keyframe)> callback );
    ~MPP(){
        if(cfg) mpp_enc_cfg_deinit(cfg);
        if(ctx) mpp_destroy(ctx);
    };  

}; 

#endif // !_MPP_H
