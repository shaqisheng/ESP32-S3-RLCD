#include "todo_manager.h"

#include <algorithm>
#include <ctime>
#include <iterator>
#include <cJSON.h>
#include <esp_random.h>

#include "manager_safety.h"
#include "settings.h"

namespace {
std::string JsonString(cJSON* root, const char* key) {
    cJSON* value = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}
bool ValidDate(const std::string& value) {
    return rlcd::IsStrictIsoDate(value);
}
bool ValidTime(const std::string& value) {
    if (value.empty()) return true;
    if (value.size() != 5 || value[2] != ':') return false;
    int h = atoi(value.substr(0, 2).c_str()), m = atoi(value.substr(3, 2).c_str());
    return h >= 0 && h < 24 && m >= 0 && m < 60;
}
std::string NewId() {
    char value[16];
    snprintf(value, sizeof(value), "td_%08lx", static_cast<unsigned long>(esp_random()));
    return value;
}
void AddItem(cJSON* array, const TodoItem& item) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", item.id.c_str());
    cJSON_AddStringToObject(obj, "content", item.content.c_str());
    cJSON_AddStringToObject(obj, "due_date", item.due_date.c_str());
    cJSON_AddStringToObject(obj, "due_time", item.due_time.c_str());
    cJSON_AddBoolToObject(obj, "completed", item.completed);
    cJSON_AddNumberToObject(obj, "order", item.order);
    cJSON_AddNumberToObject(obj, "created_at", static_cast<double>(item.created_at));
    cJSON_AddNumberToObject(obj, "updated_at", static_cast<double>(item.updated_at));
    cJSON_AddItemToArray(array, obj);
}
}

TodoManager& TodoManager::GetInstance() {
    static TodoManager instance;
    return instance;
}

void TodoManager::Init() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return;
    LoadLocked();
    initialized_ = true;
}

void TodoManager::LoadLocked() {
    Settings settings("todos", false);
    std::string raw = settings.GetString("items", "");
    bool migrated = false;
    if (raw.empty()) {
        Settings old("memo", false);
        raw = old.GetString("items", "");
        migrated = !raw.empty();
    }
    cJSON* array = raw.empty() ? nullptr : cJSON_Parse(raw.c_str());
    if (!cJSON_IsArray(array)) {
        if (array) cJSON_Delete(array);
        return;
    }
    int index = 0;
    cJSON* obj = nullptr;
    cJSON_ArrayForEach(obj, array) {
        TodoItem item;
        item.id = JsonString(obj, "id");
        item.content = JsonString(obj, "content");
        item.due_date = JsonString(obj, "due_date");
        item.due_time = JsonString(obj, "due_time");
        if (item.content.empty()) {
            item.content = JsonString(obj, "c");
            item.due_time = JsonString(obj, "t");
            migrated = true;
        }
        if (item.content.empty()) continue;
        if (item.id.empty()) item.id = NewId();
        cJSON* done = cJSON_GetObjectItem(obj, "completed");
        item.completed = cJSON_IsTrue(done);
        item.order = index++;
        item.created_at = static_cast<int64_t>(cJSON_GetNumberValue(cJSON_GetObjectItem(obj, "created_at")));
        item.updated_at = static_cast<int64_t>(cJSON_GetNumberValue(cJSON_GetObjectItem(obj, "updated_at")));
        if (!item.created_at) item.created_at = time(nullptr);
        if (!item.updated_at) item.updated_at = item.created_at;
        items_.push_back(std::move(item));
    }
    cJSON_Delete(array);
    if (migrated) {
        std::string ignored;
        SaveLocked(ignored);
    }
}

std::vector<TodoItem> TodoManager::List() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = items_;
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.completed != b.completed) return !a.completed;
        if (a.due_date != b.due_date) {
            if (a.due_date.empty()) return false;
            if (b.due_date.empty()) return true;
            return a.due_date < b.due_date;
        }
        if (a.due_time != b.due_time) return a.due_time < b.due_time;
        return a.order < b.order;
    });
    return result;
}

bool TodoManager::SaveItemsLocked(const std::vector<TodoItem>& items, std::string& error) {
    cJSON* array = cJSON_CreateArray();
    for (const auto& item : items) AddItem(array, item);
    char* raw = cJSON_PrintUnformatted(array);
    if (!raw) { cJSON_Delete(array); error = "内存不足"; return false; }
    Settings settings("todos", true);
    settings.SetString("items", raw);
    cJSON_free(raw);
    cJSON_Delete(array);
    return true;
}

bool TodoManager::SaveLocked(std::string& error) {
    return SaveItemsLocked(items_, error);
}

std::string TodoManager::ToJson() const {
    auto items = List();
    cJSON* root = cJSON_CreateObject();
    cJSON* array = cJSON_AddArrayToObject(root, "items");
    for (const auto& item : items) AddItem(array, item);
    char* raw = cJSON_PrintUnformatted(root);
    std::string result = raw ? raw : "{\"items\":[]}";
    if (raw) cJSON_free(raw);
    cJSON_Delete(root);
    return result;
}

bool TodoManager::Create(const char* json, TodoItem& created, std::string& error) {
    cJSON* root = cJSON_Parse(json);
    if (!root) { error = "JSON 无效"; return false; }
    created.content = JsonString(root, "content");
    created.due_date = JsonString(root, "due_date");
    created.due_time = JsonString(root, "due_time");
    cJSON_Delete(root);
    if (created.content.empty() || created.content.size() > 96) { error = "待办内容需为 1-96 字节"; return false; }
    if (!ValidDate(created.due_date) || !ValidTime(created.due_time)) { error = "日期或时间格式无效"; return false; }
    std::lock_guard<std::mutex> lock(mutex_);
    if (items_.size() >= 32) { error = "最多保存 32 条待办"; return false; }
    created.id = NewId();
    created.order = items_.size();
    created.created_at = created.updated_at = time(nullptr);
    items_.push_back(created);
    return SaveLocked(error);
}

bool TodoManager::Update(const std::string& id, const char* json, std::string& error) {
    cJSON* root = cJSON_Parse(json);
    if (!root) { error = "JSON 无效"; return false; }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(items_.begin(), items_.end(), [&](const auto& item) { return item.id == id; });
    if (it == items_.end()) { cJSON_Delete(root); error = "待办不存在"; return false; }
    const size_t index = static_cast<size_t>(std::distance(items_.begin(), it));
    const bool updated = rlcd::CommitValidatedUpdate(
        *it,
        [&](TodoItem& candidate) {
            cJSON* value = cJSON_GetObjectItem(root, "content");
            if (cJSON_IsString(value)) candidate.content = value->valuestring;
            value = cJSON_GetObjectItem(root, "due_date");
            if (cJSON_IsString(value)) candidate.due_date = value->valuestring;
            value = cJSON_GetObjectItem(root, "due_time");
            if (cJSON_IsString(value)) candidate.due_time = value->valuestring;
            value = cJSON_GetObjectItem(root, "completed");
            if (cJSON_IsBool(value)) candidate.completed = cJSON_IsTrue(value);
            value = cJSON_GetObjectItem(root, "order");
            if (cJSON_IsNumber(value)) candidate.order = value->valueint;
            candidate.updated_at = time(nullptr);
        },
        [&](const TodoItem& candidate) {
            return !candidate.content.empty() && candidate.content.size() <= 96 &&
                   ValidDate(candidate.due_date) && ValidTime(candidate.due_time);
        },
        [&](const TodoItem& candidate) {
            auto persisted_items = items_;
            persisted_items[index] = candidate;
            return SaveItemsLocked(persisted_items, error);
        });
    cJSON_Delete(root);
    if (!updated) {
        if (error.empty()) error = "待办字段无效";
        return false;
    }
    return true;
}

bool TodoManager::Remove(const std::string& id, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(items_.begin(), items_.end(), [&](const auto& item) { return item.id == id; });
    if (it == items_.end()) { error = "待办不存在"; return false; }
    items_.erase(it);
    return SaveLocked(error);
}
