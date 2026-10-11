// After the inference device has failed, releasing GPU memory must neither throw
// (destructors free, and a throw out of one is std::terminate) nor wait out the
// stall again for every buffer. The failure is a submission waiting on a
// semaphore that nothing signals until the stall watch has given up; ~10 s.

#include "nn/Device.h"
#include "nn/vk/Context.h"
#include "nn/vk/Memory.h"
#include "nn/vk/Stream.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main() {
#ifdef _WIN32
    _putenv_s("SS_GPU_STALL_SECONDS", "0.5");
#else
    setenv("SS_GPU_STALL_SECONDS", "0.5", 1);
#endif
    nn::vk::Context& ctx = nn::vk::Context::get();
    nn::vk::Stream& s = nn::vk::Stream::get();
    nn::vk::DevicePtr buf = nn::vk::device_alloc(4096, "teardown_test");
    s.zero(buf, 4096);
    s.sync();

    VkSemaphoreTypeCreateInfo sti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    sti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &sti;
    VkSemaphore gate = VK_NULL_HANDLE;
    if (vkCreateSemaphore(ctx.device(), &sci, nullptr, &gate) != VK_SUCCESS) {
        std::printf("FAIL could not create a timeline semaphore\n");
        return 1;
    }
    // Each give-up takes ~2 s at this limit; free()'s vkDeviceWaitIdle blocks
    // until the gate opens.
    std::thread opener([&] {
        std::this_thread::sleep_for(std::chrono::seconds(8));
        VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        si.semaphore = gate;
        si.value = 1;
        vkSignalSemaphore(ctx.device(), &si);
    });

    s.waitOn(gate, 1);
    s.zero(buf, 4096);
    bool threw = false;
    try {
        s.sync();
    } catch (const std::exception& e) {
        threw = true;
        std::printf("  sync: %s\n", e.what());
    }
    check(threw, "sync fails on a submission that cannot run");

    s.drain();
    check(true, "drain after the failure does not throw");
    const auto t0 = std::chrono::steady_clock::now();
    s.drain();
    const double again = seconds_since(t0);
    check(again < 0.25, "a second drain returns at once (" + std::to_string(again) + " s)");

    {
        nn::vk::Arena arena("teardown_test");
        arena.reserve(1 << 20);
    }
    check(true, "a destructor that frees after the failure does not terminate");

    opener.join();
    nn::vk::device_free(buf);
    vkDestroySemaphore(ctx.device(), gate, nullptr);
    nn::shutdown();
    check(!nn::vk::Context::initialized(), "shutdown releases the failed device");

    try {
        nn::vk::DevicePtr a = nn::vk::device_alloc(4096, "teardown_test");
        const float host[4] = {1, 2, 3, 4};
        float back[4] = {};
        nn::vk::Stream::get().upload(a, host, sizeof(host));
        nn::vk::Stream::get().download(back, a, sizeof(back));
        check(back[0] == 1 && back[3] == 4, "a fresh device round-trips data");
        nn::vk::device_free(a);
    } catch (const std::exception& e) {
        check(false, std::string("a fresh device round-trips data: ") + e.what());
    }
    nn::shutdown();

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
