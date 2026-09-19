#include "frame_limiter.h"

#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h> // timeBeginPeriod / timeEndPeriod
#endif

namespace gryce_engine::utils {

FrameLimiter::FrameLimiter() {
    frame_start_ = std::chrono::steady_clock::now();

#ifdef _WIN32
    // 把 Windows 定时器分辨率提升到 1ms，作为兜底
    timeBeginPeriod(1);

    // 尝试创建高精度可等待定时器（Windows 10 1803+），支持亚毫秒精度
    using CreateWaitableTimerExW_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto create_timer = reinterpret_cast<CreateWaitableTimerExW_t>(
        GetProcAddress(kernel32, "CreateWaitableTimerExW"));
    if (create_timer) {
        const DWORD HIGH_RES_TIMER_FLAG = 0x00000002; // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
        timer_handle_ = create_timer(nullptr, nullptr,
                                     HIGH_RES_TIMER_FLAG,
                                     TIMER_ALL_ACCESS);
        if (timer_handle_) {
            timer_high_res_ = true;
        }
    }
    if (!timer_handle_) {
        timer_handle_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    }
#endif
}

FrameLimiter::~FrameLimiter() {
#ifdef _WIN32
    if (timer_handle_) {
        CloseHandle(static_cast<HANDLE>(timer_handle_));
        timer_handle_ = nullptr;
    }
    timeEndPeriod(1);
#endif
}

// ---------------------------------------------------------------------------
// 平台相关的高精度睡眠
// ---------------------------------------------------------------------------
#ifdef _WIN32
static void precise_sleep(void* timer_handle, double seconds) {
    if (seconds <= 0.0) return;
    if (timer_handle) {
        LARGE_INTEGER due;
        // 负值表示相对时间，单位 100ns
        due.QuadPart = -static_cast<LONGLONG>(seconds * 10'000'000.0);
        if (due.QuadPart < 0) {
            SetWaitableTimer(static_cast<HANDLE>(timer_handle), &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(static_cast<HANDLE>(timer_handle), INFINITE);
            return;
        }
    }
    DWORD ms = static_cast<DWORD>(seconds * 1000.0);
    if (ms < 1) ms = 1;
    Sleep(ms);
}
#endif

void FrameLimiter::set_target_fps(int fps) {
    target_fps_.store(fps);
    if (fps <= 0) {
        target_frame_time_.store(0.0);
    } else {
        target_frame_time_.store(1.0 / static_cast<double>(fps));
    }
}

void FrameLimiter::begin_frame() {
    frame_start_ = std::chrono::steady_clock::now();
}

double FrameLimiter::end_frame() {
    using namespace std::chrono;

    auto frame_end = steady_clock::now();
    double elapsed = duration<double>(frame_end - frame_start_).count();
    last_frame_time_.store(elapsed);

    if (!enabled_.load() || target_fps_.load() <= 0) {
        return 0.0;
    }

    double target = target_frame_time_.load();
    double remaining = target - elapsed;

    if (remaining <= 0.0) {
        return 0.0;
    }

    // 最后 0.5ms 用忙等避免 sleep 抖动；其余用高精度定时器让出 CPU
    constexpr double spin_seconds = 0.0005;
    if (remaining > spin_seconds) {
        double sleep_seconds = remaining - spin_seconds;
#ifdef _WIN32
        precise_sleep(timer_handle_, sleep_seconds);
#else
        std::this_thread::sleep_for(duration<double>(sleep_seconds));
#endif
    }

    while (duration<double>(steady_clock::now() - frame_start_).count() < target) {
        // busy spin
    }

    return remaining;
}

} // namespace gryce_engine::utils
