#
# Copyright (C) 2012 The Android Open Source Project
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

# Provides overrides to configure the Dalvik heap for a 2GB phone
# 192m of RAM gives enough space for 5 8 megapixel camera bitmaps in RAM.

PRODUCT_PRODUCT_PROPERTIES += \
    dalvik.vm.heapstartsize?=8m \
    dalvik.vm.heapgrowthlimit?=192m \
    dalvik.vm.heapsize?=512m \
    dalvik.vm.heaptargetutilization?=0.75 \
    dalvik.vm.heapminfree?=2m \
    dalvik.vm.heapmaxfree?=16m \
    dalvik.vm.usejit?=true \
    dalvik.vm.jitmaxsize?=128m \
    dalvik.vm.jitinitialsize?=16m \
    dalvik.vm.jitthreshold?=15000 \
    dalvik.vm.madvise.vdexfile.size?=83886080 \
    dalvik.vm.madvise.odexfile.size?=83886080 \
    dalvik.vm.usap_pool_enabled?=true \
    dalvik.vm.usap_pool_size_min?=1 \
    dalvik.vm.usap_pool_size_max?=2 \
    dalvik.vm.usap_refill_threshold?=1 \
    dalvik.vm.usap_pool_refill_delay_ms?=3000 \
    persist.sys.pinner.quota_pct?=6
