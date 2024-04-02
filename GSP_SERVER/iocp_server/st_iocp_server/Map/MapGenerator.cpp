#include "MapGenerator.h"

vector<Node *> MapGenerator::find_path(int start_x, int start_y, int goal_x, int goal_y)
{
    auto comp = [](Node *a, Node *b) { return *a > *b; };
    priority_queue<Node *, vector<Node *>, decltype(comp)> open_set(comp);
    vector<vector<bool>> closed_set(height_, vector<bool>(width_, false));

    open_set.emplace(new Node(start_x, start_y, 0, heuristic(start_x, start_y, goal_x, goal_y)));

    while (!open_set.empty())
    {
        Node *current = open_set.top();
        open_set.pop();

        if (current->x == goal_x && current->y == goal_y)
        {
            return reconstruct_path(current);
        }

        if (closed_set[current->y][current->x])
        {
            delete current;
            continue;
        }
        
        closed_set[current->y][current->x] = true;

        vector<pair<int, int>> directions = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};
        for (const auto &dir : directions)
        {
            const int next_x = current->x + dir.first;
            const int next_y = current->y + dir.second;

            if (is_valid(next_x, next_y) && !closed_set[next_y][next_x])
            {
                auto next_node = new Node(next_x, next_y, current->cost + 1,
                    heuristic(next_x, next_y, goal_x, goal_y), current);
                
                open_set.emplace(next_node);
            }
        }

        // 메모리 해제
        while (!open_set.empty())
        {
            delete open_set.top();
            open_set.pop();
        }
    }

    return {}; // 경로를 찾지 못한 경우
}

vector<Node *> MapGenerator::reconstruct_path(Node *current)
{
    vector<Node *> path;
    while (current)
    {
        path.push_back(current);
        current = current->parent;
    }
    reverse(path.begin(), path.end());
    return path;
}
