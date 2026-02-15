#include <pthread.h>
#include <iostream>
#include <vector>
#include <thread>
#include <sched.h>
void bind_self_to_cores(const std::vector<int>& target_cores) {
    cpu_set_t mask;
    CPU_ZERO(&mask); // 清空掩码

    for (int core_id : target_cores) {
        CPU_SET(core_id, &mask); // 把这个核心加入允许列表
    }

    // 设置亲和性
    // pthread_self() 获取当前线程句柄
    int ret = pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
    
    if (ret != 0) {
        std::cerr << "Error: Failed to bind thread to cores!" << std::endl;
    } else {
        // std::cout << "Thread bound to specific cores successfully." << std::endl;
    }
}
