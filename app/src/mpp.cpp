#include "mpp.h"

bool MPP::init(int width,int height,int fps)
{
    ctx = nullptr;
    this->mpi = nullptr;
    cfg = nullptr;
    MPP_RET ret;

    ret = mpp_create(&ctx,&mpi);
    if(ret != MPP_OK)
    {
        cerr<<"[MPP create] failed"<<endl;
        return false;
    }

    ret = mpp_init(ctx,MPP_CTX_ENC,MPP_VIDEO_CodingAVC);//MPP_VIDEO_CodingAVC = h264
    if(ret != MPP_OK)
    {
        cerr<<"[MPP init] failed"<<endl;
        return false;
    }

    ret = mpp_enc_cfg_init(&cfg);
    if(ret != MPP_OK)
    {
        cerr<<"[MPP cfg] failed"<<endl;
        return false;
    }

    // 4. 暴力注入极其严格的硬件配置参数
    // 图像尺寸与格式 (YUV420SP 就是 NV12 的学名)
    mpp_enc_cfg_set_s32(cfg, "prep:width", width);
    mpp_enc_cfg_set_s32(cfg, "prep:height", height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", width);  // 步长
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", height);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);

    // 码率控制 (这里设为 2Mbps 恒定码率 CBR，保证推流不断流、不卡顿)
    int bps = 2 * 1024 * 1024; // 2 Mbps
    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bps);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bps * 17 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bps * 15 / 16);

    // // 🌟 强力锁死 QP (画质参数)！防止编码器发疯吃内存！
    // // 限制 QP 在 20~48 之间，这是安防摄像头最常用的平衡配置
    mpp_enc_cfg_set_s32(cfg, "rc:qp_init", 26);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_min", 20);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_max", 38);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", 20);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", 38);

    // 帧率控制 (设置输入和输出的 FPS)
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", 1);

    // GOP (Group of Pictures) 设置，通常为帧率的 2 倍 (2秒一个完整关键帧 I 帧)
    mpp_enc_cfg_set_s32(cfg, "rc:gop", fps * 2);
    mpp_enc_cfg_set_s32(cfg, "h264:profile", 66);

    ret = mpi->control(ctx,MPP_ENC_SET_CFG,cfg);
    if(ret != MPP_OK)
    {
        cerr<<"[MPP set cfg] failed"<<endl;
        return false;
    }
    mpp_buffer_group_get_internal(&buf_grp, MPP_BUFFER_TYPE_DRM);
    // std::cout << "🚀 MPP H.264 Encoder Init Success!" << std::endl;
    return true;
}
bool MPP::encoder(const cv::Mat& nv12_img, int width, int height, bool is_eos, 
                 std::function<void(void* ptr, size_t len, bool is_keyframe)> callback )
{
    this->frame = nullptr;
    this->packet = nullptr;
    this->buffer = nullptr;
    this->buf_grp = nullptr;
    if(nv12_img.empty() && is_eos)
    {
        mpp_frame_init(&frame);
        mpp_frame_set_eos(frame,1);
        mpi->encode_put_frame(ctx,frame);
    }
    else{
        size_t size = width * height * 3 / 2;

        mpp_buffer_get(buf_grp,&buffer,size);
        if(!buffer) return false;

        if(!nv12_img.empty())
        {
            memcpy(mpp_buffer_get_ptr(buffer),nv12_img.data,size);
        }

        mpp_frame_init(&frame);
        mpp_frame_set_width(frame,width);
        mpp_frame_set_height(frame,height);
        mpp_frame_set_hor_stride(frame,width);
        mpp_frame_set_ver_stride(frame,height);
        mpp_frame_set_fmt(frame,MPP_FMT_YUV420SP);
        mpp_frame_set_buffer(frame,buffer);

        if(is_eos)
        {
            mpp_frame_set_eos(frame,1);
        }

        mpi->encode_put_frame(ctx,frame);

    }

    

    do
    {
        MPP_RET ret;
        ret = mpi->encode_get_packet(ctx,&packet);
        if(ret == MPP_OK && packet != nullptr)
        {
            void *ptr = mpp_packet_get_pos(packet);
            size_t len = mpp_packet_get_length(packet);

            uint32_t flag = mpp_packet_get_flag(packet);
            bool is_keyframe = (flag & MPP_PACKET_FLAG_INTRA) ? true : false;

            if(ptr && len > 0 && callback)
            {
                callback(ptr,len,is_keyframe);
            }

            int is_pkt_eos = mpp_packet_get_eos(packet);
            mpp_packet_deinit(&packet);
            if(is_pkt_eos) break;
        }
        else{
            break;
        }
    } while (1);

    mpp_frame_deinit(&frame);
    mpp_buffer_put(buffer);
    return true;
    

}
