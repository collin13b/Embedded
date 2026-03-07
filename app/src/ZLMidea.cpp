#include "ZLMidea.h"
#include <cstring>
bool zl_media::media_init(int width,int height,int fps)
{
    //init enviroment
    mk_config cfg;
    memset(&cfg,0,sizeof(mk_config));
    cfg.ini_is_path = 0;
    cfg.ini = "";
    mk_env_init(&cfg);

    // start
    mk_rtsp_server_start(8554,0);
    mk_http_server_start(8080,0);
    mk_rtc_server_start(8000);
    
    //create 
    media = mk_media_create("__defaultVhost","live","test",0,0,0);
    if(!media) return false;
    // 4. 初始化 H.264 视频轨道 (0 代表 H264)
    mk_media_init_video(media,0,width,height,fps,8000000);
    mk_media_init_complete(media);

    start_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    std::cout << "🚀 ZLMediaKit 内置服务器启动成功！" << std::endl;
    return true;

}
bool zl_media::push_frame(void *data,size_t size)
{
    if(!media || !data || size ==0) return false;
    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

    static uint32_t fake_timestamp = 0;
    fake_timestamp += 33;
    mk_media_input_h264(media,data,size,fake_timestamp,fake_timestamp);
    return true;
}
