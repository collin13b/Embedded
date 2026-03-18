# 多路摄像头整合工作计划

## TL;DR

> **目标**: 整合MIPI摄像头和USB摄像头，实现左右并排双画面合成推流，两路画面均进行YOLOv5s AI推理
> 
> **核心挑战**: 
> - USB摄像头MJPEG解码 → BGR转换
> - 双路画面同步和帧率对齐
> - 2912x1224合成画面MPP硬件编码
> - 双路AI推理流水线改造
> 
> **交付物**:
> - `app/src/usb_camera.cpp` - USB摄像头读取模块
> - `app/src/frame_composer.cpp` - 画面合成模块
> - 改造后的 `app/src/main.cpp` - 支持双路输入
> - 合成画面推流: `rtmp://192.168.5.32/live/combined`
> 
> **预计工作量**: 中等（3-4小时）
> **并行度**: 高（模块独立开发）
> **依赖**: MPP编码器已就绪，YOLOv5s推理已就绪

---

## Context

### 硬件环境
- **平台**: Rockchip RK3588/RK356X
- **MIPI摄像头**: `/dev/video12`, 1632x1224, NV12, 30fps
- **USB摄像头**: `/dev/video20`, 1280x720, MJPEG, 30fps
- **NPU**: 用于YOLOv5s目标检测推理
- **MPP**: 硬件H264编码器
- **RGA**: 硬件图像格式转换

### 当前架构
```
MIPI摄像头(V4L2 NV12) → RGA转BGR → read_queue → AI推理 → write_queue → RGA转NV12 → MPP编码 → RTMP推流
```

### 目标架构
```
┌─ MIPI摄像头(1632x1224 NV12) ─┐      ┌─ 合成画面(2912x1224 BGR) ─┐
│  → RGA转BGR(1632x1224)       │      │  → RGA转NV12              │
│  → mipi_queue                │      │  → MPP编码(H264)          │
│  → AI推理(MIPI)              ├──────┤  → ZLMedia推流            │
│  → 合成左半部分              │      └───────────────────────────┘
└──────────────────────────────┘

┌─ USB摄像头(1280x720 MJPEG) ──┐
│  → OpenCV解码BGR(1280x720)   │
│  → usb_queue                 │
│  → AI推理(USB)               │
│  → 合成右半部分              │
└──────────────────────────────┘
```

### 技术约束
1. **合成分辨率**: 1632+1280=2912宽度，高度取最大1224
2. **USB摄像头格式**: MJPEG需要OpenCV软解码（无RGA硬件加速）
3. **帧同步**: 两路30fps，需对齐时间戳避免画面撕裂
4. **内存**: 双路AI推理增加NPU负载，需监控性能

---

## Work Objectives

### Core Objective
实现MIPI+USB双路摄像头并排合成推流，两路画面都进行YOLOv5s目标检测，最终合成画面通过RTMP推流。

### Concrete Deliverables
- `app/inc/usb_camera.h` - USB摄像头接口定义
- `app/src/usb_camera.cpp` - USB摄像头V4L2+OpenCV读取实现
- `app/inc/frame_composer.h` - 画面合成器接口定义
- `app/src/frame_composer.cpp` - 左右并排画面合成实现
- 改造 `app/src/main.cpp` - 集成双路输入、合成、推流

### Definition of Done
- [ ] USB摄像头能稳定读取1280x720@30fps MJPEG
- [ ] 画面合成器输出2912x1224@30fps
- [ ] 两路画面都显示AI检测结果（bbox+label）
- [ ] RTMP推流稳定，VLC/FFmpeg能正常播放
- [ ] 系统运行5分钟无内存泄漏

### Must Have
- USB摄像头MJPEG解码和BGR输出
- 双路队列管理（mipi_queue + usb_queue）
- 左右并排画面合成（MIPI左，USB右）
- 双路独立AI推理（各自在自己的画面上画框）
- 合成画面MPP硬件编码推流

### Must NOT Have (Guardrails)
- 不改动现有的YOLOv5s推理模块（仅增加输入队列）
- 不改动ZLMedia推流模块（仅修改输入分辨率）
- 不引入新的依赖库（使用现有OpenCV和V4L2）
- 不对USB摄像头使用RGA（OpenCV软解码足够）

---

## Verification Strategy

### Test Decision
- **Infrastructure exists**: NO（需要创建新的测试场景）
- **Automated tests**: NO（嵌入式系统以手动验证为主）
- **Agent QA Scenarios**: YES（每个任务包含详细验证步骤）

### QA Policy
每个任务必须通过以下方式验证：
1. **编译验证**: `cd build && make -j$(nproc)` 无错误
2. **运行时验证**: 实际运行程序检查功能
3. **推流验证**: FFmpeg拉流测试画面正确性
4. **性能验证**: 监控CPU/NPU占用率

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 1 (并行独立开发 - 无依赖):
├── Task 1: 创建USB摄像头读取模块 [quick]
└── Task 2: 创建画面合成器模块 [quick]

Wave 2 (依赖Wave 1 - 核心集成):
├── Task 3: 改造主程序支持双路输入 [unspecified-high]
├── Task 4: 实现双路AI推理流水线 [unspecified-high]
└── Task 5: 适配MPP编码器处理2912x1224 [quick]

Wave 3 (验证测试):
├── Task 6: 集成测试和调优 [deep]
└── Task 7: 性能优化和内存检查 [unspecified-high]

Wave FINAL (代码审查):
├── Task F1: 代码质量审查 [unspecified-high]
└── Task F2: 最终验证 [deep]
```

### Critical Path
Task 1 → Task 2 → Task 3 → Task 4 → Task 6 → FINAL

### Parallel Speedup
约40%（Wave 1两任务可并行）

---

## TODOs

- [ ] 1. 创建USB摄像头读取模块 (app/src/usb_camera.cpp)

  **What to do**:
  - 创建 `app/inc/usb_camera.h` 头文件，定义USBCamera类接口
  - 创建 `app/src/usb_camera.cpp` 实现V4L2+OpenCV读取
  - 支持MJPEG格式解码（使用OpenCV imdecode）
  - 输出1280x720 BGR格式Mat
  - 推入usb_queue线程安全队列
  
  **Must NOT do**:
  - 不使用RGA硬件加速（USB摄像头直接OpenCV解码）
  - 不修改现有的SafeQueue实现
  - 不引入新的线程库
  
  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Reason**: 主要是封装现有V4L2和OpenCV API，逻辑清晰
  - **Skills**: [`playwright`]（用于验证视频流）
  
  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (与Task 2并行)
  - **Blocks**: Task 3
  - **Blocked By**: None
  
  **References**:
  - `app/inc/Safequeue.h` - 线程安全队列接口
  - `app/src/main.cpp:86-256` - MIPI摄像头V4L2读取参考
  - OpenCV imdecode API: `cv::imdecode(buffer, cv::IMREAD_COLOR)` 用于MJPEG解码
  - OpenCV VideoCapture API: `cv::VideoCapture cap(device, cv::CAP_V4L2)`
  
  **Implementation Details**:
  ```cpp
  // usb_camera.h 接口设计
  class USBCamera {
  public:
      bool init(const std::string& device, int width, int height);
      bool read_frame(cv::Mat& frame);  // 输出BGR
      void release();
  private:
      cv::VideoCapture cap_;
      int width_, height_;
  };
  ```
  
  **Acceptance Criteria**:
  - [ ] 代码编译通过: `cd build && make -j$(nproc)` 无错误
  - [ ] USB摄像头能稳定打开 `/dev/video20`
  - [ ] 能连续读取30帧以上不丢帧
  - [ ] 输出Mat尺寸为1280x720，格式为CV_8UC3 (BGR)
  
  **QA Scenarios**:
  
  ```
  Scenario: USB摄像头基本读取
    Tool: Bash
    Preconditions: USB摄像头已连接 /dev/video20
    Steps:
      1. 编译项目: cd build && cmake .. && make -j$(nproc)
      2. 运行测试程序读取10帧
      3. 验证每帧尺寸: 1280x720
    Expected Result: 
      - 成功读取10帧
      - 每帧Mat.cols=1280, Mat.rows=720
      - Mat.type() == CV_8UC3
    Evidence: .sisyphus/evidence/task1-usb-read.log
  
  Scenario: USB摄像头长时间稳定性
    Tool: Bash
    Preconditions: USB摄像头已连接
    Steps:
      1. 运行测试程序读取300帧（10秒@30fps）
      2. 统计成功读取帧数和耗时
    Expected Result:
      - 成功读取帧数 >= 295（允许少量丢帧）
      - 平均帧率 >= 28fps
    Evidence: .sisyphus/evidence/task1-usb-stability.log
  ```
  
  **Evidence to Capture**:
  - [ ] 编译日志: .sisyphus/evidence/task1-compile.log
  - [ ] 帧读取测试日志: .sisyphus/evidence/task1-usb-read.log
  - [ ] 长时间稳定性日志: .sisyphus/evidence/task1-usb-stability.log
  
  **Commit**: YES
  - Message: `feat(camera): add USB camera reader with MJPEG decode`
  - Files: `app/inc/usb_camera.h`, `app/src/usb_camera.cpp`
  - Pre-commit: `cd build && make -j$(nproc)`

- [ ] 2. 创建画面合成器模块 (app/src/frame_composer.cpp)

  **What to do**:
  - 创建 `app/inc/frame_composer.h` 头文件
  - 创建 `app/src/frame_composer.cpp` 实现画面合成
  - 实现左右并排布局：MIPI(1632x1224) + USB(1280x720)
  - 输出合成画面 2912x1224 BGR格式
  - 处理高度不一致：USB画面垂直居中或顶部对齐
  
  **Must NOT do**:
  - 不使用RGA硬件合成（简化实现，先用OpenCV hconcat）
  - 不进行AI推理（只负责画面合成）
  - 不处理音频
  
  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Reason**: 主要是OpenCV图像操作，逻辑简单
  
  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (与Task 1并行)
  - **Blocks**: Task 3
  - **Blocked By**: None
  
  **References**:
  - OpenCV hconcat: `cv::hconcat(src1, src2, dst)` - 水平拼接两张图
  - OpenCV resize: `cv::resize(src, dst, Size(width, height))` - 调整USB画面高度
  - `app/inc/RGA.h` - 现有RGA封装（后续可优化为硬件合成）
  
  **Implementation Details**:
  ```cpp
  // frame_composer.h 接口设计
  class FrameComposer {
  public:
      // 初始化合成器，指定输出分辨率
      bool init(int mipi_width, int mipi_height, int usb_width, int usb_height);
      
      // 合成两路画面
      // mipi_frame: 1632x1224 BGR
      // usb_frame: 1280x720 BGR
      // output: 2912x1224 BGR (左右并排)
      bool compose(const cv::Mat& mipi_frame, const cv::Mat& usb_frame, cv::Mat& output);
      
  private:
      int mipi_w_, mipi_h_, usb_w_, usb_h_;
      int out_w_, out_h_;
  };
  ```
  
  **高度对齐策略**:
  - MIPI高度1224，USB高度720
  - 方案A：USB画面resize到1280x1224（保持比例会变形）❌
  - 方案B：USB画面保持1280x720，在1224高度中垂直居中（上下黑边）✅
  - 方案C：USB画面resize到1280x1224（裁剪模式）
  - **推荐方案B**：保持画面比例，视觉效果最佳
  
  **Acceptance Criteria**:
  - [ ] 合成画面宽度 = 1632 + 1280 = 2912
  - [ ] 合成画面高度 = 1224（取MIPI高度）
  - [ ] USB画面在合成画面中垂直居中
  - [ ] 合成帧率 >= 25fps
  
  **QA Scenarios**:
  
  ```
  Scenario: 画面合成尺寸验证
    Tool: Bash
    Preconditions: 已准备测试图片 test_mipi.jpg (1632x1224) 和 test_usb.jpg (1280x720)
    Steps:
      1. 调用FrameComposer::compose合成画面
      2. 验证输出Mat尺寸
      3. 保存合成图片到 disk 人工检查布局
    Expected Result:
      - output.cols == 2912
      - output.rows == 1224
      - 左侧是MIPI画面，右侧是USB画面
      - USB画面垂直居中（上下各252像素黑边）
    Evidence: .sisyphus/evidence/task2-compose-result.jpg
  
  Scenario: 合成性能测试
    Tool: Bash
    Steps:
      1. 循环合成100帧
      2. 统计总耗时
    Expected Result:
      - 总耗时 <= 4秒（即 >= 25fps）
    Evidence: .sisyphus/evidence/task2-compose-perf.log
  ```
  
  **Evidence to Capture**:
  - [ ] 合成画面截图: .sisyphus/evidence/task2-compose-result.jpg
  - [ ] 性能测试日志: .sisyphus/evidence/task2-compose-perf.log
  
  **Commit**: YES
  - Message: `feat(composer): add side-by-side frame composer`
  - Files: `app/inc/frame_composer.h`, `app/src/frame_composer.cpp`
  - Pre-commit: `cd build && make -j$(nproc)`

