#include "RGA.h"


void rga(int )
{
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
}


