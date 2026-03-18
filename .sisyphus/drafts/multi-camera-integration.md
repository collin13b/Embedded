# 多路摄像头整合方案草稿

## 当前项目状态

### 硬件平台
- Rockchip RK3588/RK356X
- MPP (Media Process Platform) 硬件编码
- RGA (Rockchip Graphics Accelerator) 硬件图像处理
- NPU 用于 YOLOv5s 推理

### 当前实现
1. **MIPI摄像头** (`/dev/video12`)
   - V4L2 MPLANE 捕获
   - DMA buffer (零拷贝)
   - NV12 格式
   - 分辨率: 1632x1224
   - RGA 硬件转码 NV12 → BGR
   - 已完整实现 Read_func

2. **USB摄像头** (`/dev/video20`)
   - 设备已打开 (main.cpp:443)
   - 但 USB_Read_func 是空的 (main.cpp:257-260)
   - 等待实现

### 处理流水线
```
[MIPI摄像头] → V4L2捕获 → RGA转码 → read_queue → AI推理(线程池) → write_queue → RGA编码 → MPP H264编码 → RTMP推流
```

### 关键代码位置
- 主函数: `app/src/main.cpp`
- MPP编码器: `app/src/mpp.cpp`
- ZLMedia推流: `app/src/ZLMidea.cpp`
- 线程池推理: `app/src/thread_pool.cpp`

### 待决策问题
1. USB摄像头的使用方式
2. USB摄像头的格式和分辨率
3. 多路推流策略
4. 是否需要两路都进行AI推理
