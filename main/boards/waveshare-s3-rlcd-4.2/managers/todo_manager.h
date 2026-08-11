#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct TodoItem {
    std::string id;
    std::string content;
    std::string due_date;
    std::string due_time;
    bool completed = false;
    int order = 0;
    int64_t created_at = 0;
    int64_t updated_at = 0;
};

class TodoManager {
public:
    static TodoManager& GetInstance();
    void Init();
    std::vector<TodoItem> List() const;
    std::string ToJson() const;
    bool Create(const char* json, TodoItem& created, std::string& error);
    bool Update(const std::string& id, const char* json, std::string& error);
    bool Remove(const std::string& id, std::string& error);

private:
    TodoManager() = default;
    mutable std::mutex mutex_;
    std::vector<TodoItem> items_;
    bool initialized_ = false;
    void LoadLocked();
    bool SaveItemsLocked(const std::vector<TodoItem>& items, std::string& error);
    bool SaveLocked(std::string& error);
};
