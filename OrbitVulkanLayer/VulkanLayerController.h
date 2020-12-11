// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_VULKAN_LAYER_LAYER_LOGIC_H_
#define ORBIT_VULKAN_LAYER_LAYER_LOGIC_H_

#include "OrbitBase/Logging.h"
#include "OrbitService/ProducerSideChannel.h"
#include "VulkanLayerProducerImpl.h"
#include "absl/base/casts.h"
#include "vulkan/vk_layer.h"
#include "vulkan/vulkan.h"

namespace orbit_vulkan_layer {

/**
 * This class controls the logic of this layer. For the instrumented vulkan functions,
 * it provides PreCall*, PostCall* and Call* functions, where the Call* functions just forward
 * to the next layer (using the dispatch table).
 * PreCall* functions are executed before the `actual` vulkan call and PostCall* afterwards.
 * PreCall/PostCall are omitted when not needed.
 *
 * Usage: For an instrumented vulkan function "X" a common pattern from the layers entry (Main.cpp)
 * would be:
 * ```
 * logic_.PreCallX(...);
 * logic_.CallX(...);
 * logic_.PostCallX(...);
 * ```
 */
template <class DispatchTable, class QueueManager, class DeviceManager, class TimerQueryPool,
          class SubmissionTracker>
class VulkanLayerController {
 public:
  // layer metadata
  static constexpr const char* const kLayerName = "ORBIT_VK_LAYER";
  static constexpr const char* const kLayerDescription =
      "Provides GPU insights for the Orbit Profiler";

  static constexpr const uint32_t kLayerImplVersion = 1;
  static constexpr const uint32_t kLayerSpecVersion = VK_API_VERSION_1_1;

  static constexpr std::array<VkExtensionProperties, 3> kDeviceExtensions = {
      VkExtensionProperties{.extensionName = VK_EXT_DEBUG_MARKER_EXTENSION_NAME,
                            .specVersion = VK_EXT_DEBUG_MARKER_SPEC_VERSION},
      VkExtensionProperties{.extensionName = VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
                            .specVersion = VK_EXT_DEBUG_UTILS_SPEC_VERSION},
      VkExtensionProperties{.extensionName = VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME,
                            .specVersion = VK_EXT_HOST_QUERY_RESET_SPEC_VERSION}};

  VulkanLayerController()
      : device_manager_(&dispatch_table_),
        timer_query_pool_(&dispatch_table_, kNumTimerQuerySlots),
        command_buffer_manager_(&dispatch_table_, &timer_query_pool_, &device_manager_,
                                std::numeric_limits<uint32_t>::max()) {}

  ~VulkanLayerController() { CloseVulkanLayerProducerIfNecessary(); }

  // ----------------------------------------------------------------------------
  // Layer bootstrapping code
  // ----------------------------------------------------------------------------

  [[nodiscard]] VkResult OnCreateInstance(const VkInstanceCreateInfo* create_info,
                                          const VkAllocationCallbacks* allocator,
                                          VkInstance* instance) {
    InitVulkanLayerProducerIfNecessary();

    auto* layer_create_info = absl::bit_cast<VkLayerInstanceCreateInfo*>(create_info->pNext);

    while (layer_create_info != nullptr &&
           (layer_create_info->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
            layer_create_info->function != VK_LAYER_LINK_INFO)) {
      layer_create_info = absl::bit_cast<VkLayerInstanceCreateInfo*>(layer_create_info->pNext);
    }

    if (layer_create_info == nullptr) {
      return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr next_get_instance_proc_addr_function =
        layer_create_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;

    // Advance linkage for next layer
    layer_create_info->u.pLayerInfo = layer_create_info->u.pLayerInfo->pNext;

    // Need to call vkCreateInstance down the chain to actually create the
    // instance, as we need it to be alive in the create instance dispatch table.
    auto create_instance = absl::bit_cast<PFN_vkCreateInstance>(
        next_get_instance_proc_addr_function(VK_NULL_HANDLE, "vkCreateInstance"));
    VkResult result = create_instance(create_info, allocator, instance);

    dispatch_table_.CreateInstanceDispatchTable(*instance, next_get_instance_proc_addr_function);

    return result;
  }

  [[nodiscard]] VkResult OnCreateDevice(VkPhysicalDevice /*physical_device*/ physical_device,
                                        const VkDeviceCreateInfo* create_info,
                                        const VkAllocationCallbacks* allocator /*allocator*/,
                                        VkDevice* device) {
    auto* layer_create_info = absl::bit_cast<VkLayerDeviceCreateInfo*>(create_info->pNext);

    while (layer_create_info != nullptr &&
           (layer_create_info->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
            layer_create_info->function != VK_LAYER_LINK_INFO)) {
      layer_create_info = absl::bit_cast<VkLayerDeviceCreateInfo*>(layer_create_info->pNext);
    }

    if (layer_create_info == nullptr) {
      return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr next_get_instance_proc_addr_function =
        layer_create_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_get_device_proc_addr_function =
        layer_create_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;

    // Advance linkage for next layer
    layer_create_info->u.pLayerInfo = layer_create_info->u.pLayerInfo->pNext;

    // Need to call vkCreateInstance down the chain to actually create the
    // instance, as we need it to be alive in the create instance dispatch table.
    auto create_device_function = absl::bit_cast<PFN_vkCreateDevice>(
        next_get_instance_proc_addr_function(VK_NULL_HANDLE, "vkCreateDevice"));
    VkResult result = create_device_function(physical_device, create_info, allocator, device);

    dispatch_table_.CreateDeviceDispatchTable(*device, next_get_device_proc_addr_function);

    device_manager_.TrackLogicalDevice(physical_device, *device);
    timer_query_pool_.InitializeTimerQueryPool(*device);

    return result;
  }

  [[nodiscard]] PFN_vkVoidFunction OnGetDeviceProcAddr(VkDevice device, const char* name) {
    return dispatch_table_.GetDeviceProcAddr(device)(device, name);
  }

  [[nodiscard]] PFN_vkVoidFunction OnGetInstanceProcAddr(VkInstance instance, const char* name) {
    return dispatch_table_.GetInstanceProcAddr(instance)(instance, name);
  }

  void OnDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    PFN_vkDestroyInstance destroy_instance_function = dispatch_table_.DestroyInstance(instance);
    CHECK(destroy_instance_function != nullptr);
    dispatch_table_.RemoveInstanceDispatchTable(instance);

    destroy_instance_function(instance, allocator);

    CloseVulkanLayerProducerIfNecessary();
  }

  void OnDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    PFN_vkDestroyDevice destroy_device_function = dispatch_table_.DestroyDevice(device);
    CHECK(destroy_device_function != nullptr);
    device_manager_.UntrackLogicalDevice(device);
    dispatch_table_.RemoveDeviceDispatchTable(device);

    destroy_device_function(device, allocator);
  }

  // ----------------------------------------------------------------------------
  // Core layer logic
  // ----------------------------------------------------------------------------

  [[nodiscard]] VkResult OnResetCommandPool(VkDevice device, VkCommandPool command_pool,
                                            VkCommandPoolResetFlags flags) {
    VkResult result = dispatch_table_.ResetCommandPool(device)(device, command_pool, flags);
    command_buffer_manager_.ResetCommandPool(command_pool);
    return result;
  }

  [[nodiscard]] VkResult OnAllocateCommandBuffers(VkDevice device,
                                                  const VkCommandBufferAllocateInfo* allocate_info,
                                                  VkCommandBuffer* command_buffers) {
    VkResult result =
        dispatch_table_.AllocateCommandBuffers(device)(device, allocate_info, command_buffers);

    VkCommandPool pool = allocate_info->commandPool;
    const uint32_t command_buffer_count = allocate_info->commandBufferCount;
    command_buffer_manager_.TrackCommandBuffers(device, pool, command_buffers,
                                                command_buffer_count);
    return result;
  }

  void OnFreeCommandBuffers(VkDevice device, VkCommandPool command_pool,
                            uint32_t command_buffer_count, const VkCommandBuffer* command_buffers) {
    command_buffer_manager_.UntrackCommandBuffers(device, command_pool, command_buffers,
                                                  command_buffer_count);
    return dispatch_table_.FreeCommandBuffers(device)(device, command_pool, command_buffer_count,
                                                      command_buffers);
  }

  [[nodiscard]] VkResult OnBeginCommandBuffer(VkCommandBuffer command_buffer,
                                              const VkCommandBufferBeginInfo* begin_info) {
    VkResult result =
        dispatch_table_.BeginCommandBuffer(command_buffer)(command_buffer, begin_info);
    command_buffer_manager_.MarkCommandBufferBegin(command_buffer);
    return result;
  }

  [[nodiscard]] VkResult OnEndCommandBuffer(VkCommandBuffer command_buffer) {
    command_buffer_manager_.MarkCommandBufferEnd(command_buffer);
    return dispatch_table_.EndCommandBuffer(command_buffer)(command_buffer);
  }

  [[nodiscard]] VkResult OnResetCommandBuffer(VkCommandBuffer command_buffer,
                                              VkCommandBufferResetFlags flags) {
    command_buffer_manager_.ResetCommandBuffer(command_buffer);
    return dispatch_table_.ResetCommandBuffer(command_buffer)(command_buffer, flags);
  }

  void OnGetDeviceQueue(VkDevice device, uint32_t queue_family_index, uint32_t queue_index,
                        VkQueue* queue) {
    dispatch_table_.GetDeviceQueue(device)(device, queue_family_index, queue_index, queue);
    queue_manager_.TrackQueue(*queue, device);
  }

  void OnGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* queue_info, VkQueue* queue) {
    dispatch_table_.GetDeviceQueue2(device)(device, queue_info, queue);
    queue_manager_.TrackQueue(*queue, device);
  }

  [[nodiscard]] VkResult OnQueueSubmit(VkQueue queue, uint32_t submit_count,
                                       const VkSubmitInfo* submits, VkFence fence) {
    auto queue_submission_optional =
        command_buffer_manager_.PersistCommandBuffersOnSubmit(submit_count, submits);
    VkResult result = dispatch_table_.QueueSubmit(queue)(queue, submit_count, submits, fence);
    command_buffer_manager_.PersistDebugMarkersOnSubmit(queue, submit_count, submits,
                                                        queue_submission_optional);
    return result;
  }

  [[nodiscard]] VkResult OnQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* present_info) {
    command_buffer_manager_.CompleteSubmits(queue_manager_.GetDeviceOfQueue(queue));
    return dispatch_table_.QueuePresentKHR(queue)(queue, present_info);
  }

  void OnCmdBeginDebugUtilsLabelEXT(VkCommandBuffer command_buffer,
                                    const VkDebugUtilsLabelEXT* label_info) {
    if (dispatch_table_.IsDebugUtilsExtensionSupported(command_buffer)) {
      dispatch_table_.CmdBeginDebugUtilsLabelEXT(command_buffer)(command_buffer, label_info);
    }

    CHECK(label_info != nullptr);
    command_buffer_manager_.MarkDebugMarkerBegin(command_buffer, label_info->pLabelName,
                                                 {
                                                     .red = label_info->color[0],
                                                     .green = label_info->color[1],
                                                     .blue = label_info->color[2],
                                                     .alpha = label_info->color[3],
                                                 });
  }

  void OnCmdEndDebugUtilsLabelEXT(VkCommandBuffer command_buffer) {
    command_buffer_manager_.MarkDebugMarkerEnd(command_buffer);
    if (dispatch_table_.IsDebugUtilsExtensionSupported(command_buffer)) {
      dispatch_table_.CmdEndDebugUtilsLabelEXT(command_buffer)(command_buffer);
    }
  }

  void OnCmdDebugMarkerBeginEXT(VkCommandBuffer command_buffer,
                                const VkDebugMarkerMarkerInfoEXT* marker_info) {
    if (dispatch_table_.IsDebugMarkerExtensionSupported(command_buffer)) {
      dispatch_table_.CmdDebugMarkerBeginEXT(command_buffer)(command_buffer, marker_info);
    }
    CHECK(marker_info != nullptr);
    command_buffer_manager_.MarkDebugMarkerBegin(command_buffer, marker_info->pMarkerName,
                                                 {
                                                     .red = marker_info->color[0],
                                                     .green = marker_info->color[1],
                                                     .blue = marker_info->color[2],
                                                     .alpha = marker_info->color[3],
                                                 });
  }

  void OnCmdDebugMarkerEndEXT(VkCommandBuffer command_buffer) {
    command_buffer_manager_.MarkDebugMarkerEnd(command_buffer);
    if (dispatch_table_.IsDebugMarkerExtensionSupported(command_buffer)) {
      dispatch_table_.CmdDebugMarkerEndEXT(command_buffer)(command_buffer);
    }
  }

  // ----------------------------------------------------------------------------
  // Layer enumeration functions
  // ----------------------------------------------------------------------------

  [[nodiscard]] VkResult OnEnumerateInstanceLayerProperties(uint32_t* property_count,
                                                            VkLayerProperties* properties) {
    // Vulkan spec dictates that we are only supposed to enumerate ourselve.
    if (property_count != nullptr) {
      *property_count = 1;
    }
    if (properties != nullptr) {
      snprintf(properties->layerName, sizeof(properties->layerName), kLayerName);
      snprintf(properties->description, sizeof(properties->description), kLayerDescription);
      properties->implementationVersion = kLayerImplVersion;
      properties->specVersion = kLayerSpecVersion;
    }

    return VK_SUCCESS;
  }

  [[nodiscard]] VkResult OnEnumerateInstanceExtensionProperties(
      const char* layer_name, uint32_t* property_count, VkExtensionProperties* /*properties*/) {
    // Inform the client that we have no extension properties if this layer
    // specifically is being queried.
    if (layer_name != nullptr && strcmp(layer_name, kLayerName) == 0) {
      if (property_count != nullptr) {
        *property_count = 0;
      }
      return VK_SUCCESS;
    }

    // Vulkan spec mandates returning this when this layer isn't being queried.
    return VK_ERROR_LAYER_NOT_PRESENT;
  }

  [[nodiscard]] VkResult OnEnumerateDeviceExtensionProperties(VkPhysicalDevice physical_device,
                                                              const char* layer_name,
                                                              uint32_t* property_count,
                                                              VkExtensionProperties* properties) {
    // If our layer is queried exclusively, we just return our extensions
    if (layer_name != nullptr && strcmp(layer_name, kLayerName) == 0) {
      // If properties == nullptr, only the number of extensions are queried.
      if (properties == nullptr) {
        *property_count = kDeviceExtensions.size();
        return VK_SUCCESS;
      }
      uint32_t num_extensions_to_copy = kDeviceExtensions.size();
      // In the case that less extensions are queried then the layer uses, we copy on this number
      // and return VK_INCOMPLETE, according to the specification.
      if (*property_count < num_extensions_to_copy) {
        num_extensions_to_copy = *property_count;
      }
      memcpy(properties, kDeviceExtensions.data(),
             num_extensions_to_copy * sizeof(VkExtensionProperties));
      *property_count = num_extensions_to_copy;

      if (num_extensions_to_copy < kDeviceExtensions.size()) {
        return VK_INCOMPLETE;
      }
      return VK_SUCCESS;
    }

    // If a different layer is queried exclusively, we forward the call.
    if (layer_name != nullptr) {
      return dispatch_table_.EnumerateDeviceExtensionProperties(physical_device)(
          physical_device, layer_name, property_count, properties);
    }

    // This is a general query, so we need to append our extensions to the once down in the
    // callchain.
    uint32_t num_other_extensions;
    VkResult result = dispatch_table_.EnumerateDeviceExtensionProperties(physical_device)(
        physical_device, nullptr, &num_other_extensions, nullptr);
    if (result != VK_SUCCESS) {
      return result;
    }

    std::vector<VkExtensionProperties> extensions(num_other_extensions);
    result = dispatch_table_.EnumerateDeviceExtensionProperties(physical_device)(
        physical_device, nullptr, &num_other_extensions, extensions.data());
    if (result != VK_SUCCESS) {
      return result;
    }

    // Lets append all of our extensions, that are not yet listed.
    // Note, as this list of our extensions is very small, we are fine with O(N*M) runtime.
    for (const auto& extension : kDeviceExtensions) {
      bool is_unique = true;
      for (const auto& other_extension : extensions) {
        if (strcmp(extension.extensionName, other_extension.extensionName) == 0) {
          is_unique = false;
          break;
        }
      }
      if (is_unique) {
        extensions.push_back(extension);
      }
    }

    // As above, if properties is nullptr, only the number if extensions is queried.
    if (properties == nullptr) {
      *property_count = extensions.size();
      return VK_SUCCESS;
    }

    uint32_t num_extensions_to_copy = extensions.size();
    // In the case that less extensions are queried then the layer uses, we copy on this number and
    // return VK_INCOMPLETE, according to the specification.
    if (*property_count < num_extensions_to_copy) {
      num_extensions_to_copy = *property_count;
    }
    memcpy(properties, extensions.data(), num_extensions_to_copy * sizeof(VkExtensionProperties));
    *property_count = num_extensions_to_copy;

    if (num_extensions_to_copy < extensions.size()) {
      return VK_INCOMPLETE;
    }
    return VK_SUCCESS;
  }

  [[nodiscard]] const DispatchTable* dispatch_table() const { return &dispatch_table_; }

 private:
  void InitVulkanLayerProducerIfNecessary() {
    absl::MutexLock lock{&vulkan_layer_producer_mutex_};
    if (vulkan_layer_producer_ == nullptr) {
      vulkan_layer_producer_ = std::make_unique<VulkanLayerProducerImpl>();
      vulkan_layer_producer_->BringUp(orbit_service::CreateProducerSideChannel());
      command_buffer_manager_.SetVulkanLayerProducer(vulkan_layer_producer_.get());
    }
  }

  void CloseVulkanLayerProducerIfNecessary() {
    absl::MutexLock lock{&vulkan_layer_producer_mutex_};
    if (vulkan_layer_producer_ != nullptr) {
      // TODO: Only do this when DestroyInstance has been called a number of times
      //  equal to the number of times CreateInstance was called.
      LOG("Taking down VulkanLayerProducer");
      vulkan_layer_producer_->TakeDown();
      vulkan_layer_producer_.reset();
      command_buffer_manager_.SetVulkanLayerProducer(nullptr);
    }
  }

  std::unique_ptr<VulkanLayerProducer> vulkan_layer_producer_ = nullptr;
  absl::Mutex vulkan_layer_producer_mutex_;

  DispatchTable dispatch_table_;
  DeviceManager device_manager_;
  TimerQueryPool timer_query_pool_;
  SubmissionTracker command_buffer_manager_;
  QueueManager queue_manager_;

  static constexpr uint32_t kNumTimerQuerySlots = 65536;
};

}  // namespace orbit_vulkan_layer

#endif  // ORBIT_VULKAN_LAYER_LAYER_LOGIC_H_
