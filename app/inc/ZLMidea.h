#ifndef _ZLMEDIA_H
#define _ZLMEDIA_H
#include "mk_media.h"
#include <iostream>
#include <chrono>
class zl_media
{
public:
    zl_media(){};
    uint64_t start_time;
    mk_media media;
    bool media_init(int width,int height,int fps);
    bool push_frame(void *data,size_t size);
    ~zl_media(){ 
        if(media) mk_media_release(media);
        mk_stop_all_server();
        

    };

};

#endif // !_ZLMEDIA_H
