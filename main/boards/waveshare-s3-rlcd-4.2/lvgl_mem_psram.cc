// LVGL 9 自定义内存分配器：把 LVGL 全部动态内存（对象/样式/字符串/渲染临时块）重定向到 PSRAM。
//
// 背景：CONFIG_LV_USE_CLIB_MALLOC 时 LVGL 走标准 malloc → 内部 SRAM（默认 8BIT 能力优先内部）。
// 本板 6 个页面的 LVGL 对象长期占用内部 SRAM，曾出现 10 个 label 把剩余 SRAM 压到 1219B。
// 内部 SRAM 只剩约 3%，而 PSRAM 有 8MB 充裕空间。
//
// 安全性：LVGL 分配的内存不参与 SPI DMA——刷屏缓冲区是 custom_lcd_display.cc:86
// 显式 heap_caps_malloc(MALLOC_CAP_SPIRAM) 的全屏 RGB565 buffer，flush 路径（Lvgl_flush_cb
// → RlcdDriver）也不经过 LVGL 堆。因此 LVGL 堆整体放 PSRAM 是安全的。
//
// 配合 sdkconfig：CONFIG_LV_USE_CUSTOM_MALLOC=y（替代 CONFIG_LV_USE_CLIB_MALLOC）。
// LVGL 9.4 要求外部实现以下三个符号（见 managed_components/lvgl__lvgl/src/stdlib/lv_mem.h:122-135）。

#include <esp_heap_caps.h>

#include <cstddef>

extern "C" {

// 使用系统堆（heap_caps）作为后端，无需初始化/反初始化内存池，
// 但 LVGL 9.4 的 lv_init/lv_deinit 会引用这两个符号（lv_init.c:140/470），必须提供空实现。
void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

void* lv_malloc_core(size_t size) {
    // malloc(0) 在 newlib 下返回非空最小块，heap_caps_malloc(0) 返回 NULL 会触发
    // LVGL 的 LV_USE_ASSERT_MALLOC，统一按 1 字节处理对齐 clib 语义。
    // 不做内部 SRAM 兜底：兜底会把问题悄悄带回内部 SRAM，宁可分配失败暴露出来。
    return heap_caps_malloc(size ? size : 1, MALLOC_CAP_SPIRAM);
}

void lv_free_core(void* p) {
    // heap_caps_malloc 的内存可以直接用标准 free 释放
    free(p);
}

void* lv_realloc_core(void* p, size_t new_size) {
    if (!p) return lv_malloc_core(new_size);
    return heap_caps_realloc(p, new_size ? new_size : 1, MALLOC_CAP_SPIRAM);
}

}  // extern "C"
