#ifndef _RGA_H_
#define _RGA_H_
#include "yolov5s.h"
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
void nv12_to_bgr(Mat &img);
void bgr_to_nv12(Mat &img);
#endif // !_RGA_H_
