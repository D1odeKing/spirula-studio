#pragma once

// Windows + NVIDIA: a process's first vkCreateDevice faults inside nvoglv64.dll
// when a GL context already exists, seen only with several NVIDIA GPUs
// installed (issues #115, #146). The GUI's window is GL and its Vulkan
// contexts are lazy, so it creates a throwaway device per NVIDIA GPU first.

namespace gui {

// Call before glfwInit(). No-op off Windows, in a build without Vulkan, and
// with SS_GUI_VK_PRIME=0; SS_GUI_VK_PRIME=keep leaves the devices alive.
void prime_vulkan_driver();

}  // namespace gui
