#include "ZLMidea.h"


bool zl_media::global_init()
{
    static bool is_inited = false;
    if (is_inited) return true; // 防止重复初始化

    // 初始化环境
    mk_config cfg;
    memset(&cfg, 0, sizeof(mk_config));
    cfg.ini_is_path = 0;
    cfg.ini = "";
    mk_env_init(&cfg);

    // 启动各项服务 (绑定端口)
    mk_rtsp_server_start(8554, 0);
    mk_http_server_start(8080, 0);
    mk_rtc_server_start(8000);
    
    is_inited = true;
    std::cout << "🚀 ZLMediaKit 全局服务器启动成功！监听 8554 端口" << std::endl;
    return true;
}
bool zl_media::media_init(int width,int height,int fps,const string& stream_name)
{
    //init enviroment
   
    
    //create 
    // 🌟 修复：前后必须都是双下划线！
    media = mk_media_create("__defaultVhost__", "live", stream_name.c_str(), 0, 0, 0);   
    if(!media) return false;
    // 4. 初始化 H.264 视频轨道 (0 代表 H264)
    mk_media_init_video(media,0,width,height,fps,2000000);
    mk_media_init_complete(media);

    start_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    std::cout << "🚀 ZLMediaKit 内置服务器启动成功！" << std::endl;
    return true;

}
bool zl_media::push_frame(void *data,size_t size,uint32_t time)
{
    if(!media || !data || size ==0) return false;
    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

    // static uint32_t fake_timestamp = 0;
    // fake_timestamp += 33;
    mk_media_input_h264(media,data,size,time,time);
    return true;
}
