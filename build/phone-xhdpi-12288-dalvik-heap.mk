#
# Copyright 2025 AxionOS
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Provides overrides to configure the Dalvik heap for a 12GB phone

PRODUCT_PRODUCT_PROPERTIES += \
    dalvik.vm.heapstartsize?=24m \
    dalvik.vm.heapgrowthlimit?=512m \
    dalvik.vm.heapsize?=512m \
    dalvik.vm.heaptargetutilization?=0.75 \
    dalvik.vm.heapminfree?=8m \
    dalvik.vm.heapmaxfree?=96m \
    dalvik.vm.foreground-heap-growth-multiplier?=1.0 \
    dalvik.vm.enable_time_based_gc_trigger?=true \
    dalvik.vm.usejit?=true \
    dalvik.vm.jitmaxsize?=512m \
    dalvik.vm.jitinitialsize?=64m \
    dalvik.vm.jitthreshold?=5000 \
    dalvik.vm.parallel-image-loading?=true \
    dalvik.vm.madvise.vdexfile.size?=209715200 \
    dalvik.vm.madvise.odexfile.size?=209715200 \
    dalvik.vm.usap_pool_enabled?=true \
    dalvik.vm.usap_pool_size_min?=1 \
    dalvik.vm.usap_pool_size_max?=5 \
    dalvik.vm.usap_refill_threshold?=1 \
    dalvik.vm.usap_pool_refill_delay_ms?=3000
