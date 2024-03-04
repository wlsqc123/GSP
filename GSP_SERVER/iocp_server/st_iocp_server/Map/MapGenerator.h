#pragma once
#include <iostream>
#include <vector>
#include <queue>
#include <functional> // for std::greater
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <algorithm> // for std::reverse

using namespace std;

struct Node {
    int x, y;
    int cost, h;
    Node* parent;

    Node(int x, int y, int cost = 0, int h = 0, Node* parent = nullptr)
        : x(x), y(y), cost(cost), h(h), parent(parent) {}

    int f() const { return cost + h; }

    bool operator>(const Node& other) const {
        return f() > other.f();
    }
};

class MapGenerator {
public:
    MapGenerator(const int width, const int height, const float obstacle_ratio)
        : width_(width), height_(height), obstacle_ratio_(obstacle_ratio) {
        srand(static_cast<unsigned int>(time(nullptr)));
        generate_map();
    }

    void generate_map() {
        map_.clear();
        map_.resize(height_, vector<int>(width_, 0));

        int obstacle_count = static_cast<int>(width_ * height_ * obstacle_ratio_);

        while (obstacle_count > 0) {
            const int x = rand() % width_;
            const int y = rand() % height_;

            if (map_[y][x] == 0) {
                map_[y][x] = 1; // 장애물로 설정
                --obstacle_count;
            }
        }
    }

    void print_map() const {
        for (const auto& row : map_) {
            for (const int cell : row) {
                cout << (cell == 1 ? "X" : ".") << " ";
            }
            cout << "\n";
        }
    }

    // A* 알고리즘
    vector<Node*> find_path(int start_x, int start_y, int goal_x, int goal_y);

private:
    int width_, height_;
    float obstacle_ratio_;
    vector<vector<int>> map_;

    // 맨해튼 거리
    int heuristic(int x, int y, int goalX, int goalY) const {
        return abs(x - goalX) + abs(y - goalY);
    }

    // 해당 위치가 유효한지
    bool is_valid(int x, int y) const {
        return x >= 0 && x < width_ && y >= 0 && y < height_ && map_[y][x] == 0;
    }

    // 경로 재구성 함수
    static vector<Node*> reconstruct_path(Node* current);
};
