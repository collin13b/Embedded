#include <iostream>
#include <vector>
#include <ranges>
#include <queue>
#include <opencv2/opencv.hpp>
using namespace std;
static queue<int> q;
int main(void) {
    std::cout << "Hello, World!" << std::endl;
    vector<int> a = {1, 2, 3, 4, 5};
    for(auto [index,valuue]: views::enumerate(a)) {
        q.emplace(valuue);
        cout << index << ":" << valuue << endl;
    }
    std::cout << "-----" << std::endl;
    printf("queue size:%lu\n",q.size());
    for(auto[index, valuue]: views::enumerate(a)) {
        cout << index << ":" << valuue << endl;
        q.pop();
        }
        return 0;
}
