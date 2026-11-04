/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
/* C 化（2026-10-31，XiaoZhiCam）：去掉 C++ namespace，符号直接用 kiss_fft_*
 * 全局名。本固件只链 16 位定点这一份，不存在与其它精度冲突的问题。
 * 同时去掉 `#undef KISS_FFT_H`：保留头文件守卫，避免后续 include 在
 * FIXED_POINT 已撤销时把 kiss_fft_scalar 重定义成 float。
 * 原始 kiss_fft.h / tools/kiss_fftr.h 未改动。 */

#ifndef TENSORFLOW_LITE_EXPERIMENTAL_MICROFRONTEND_LIB_KISS_FFT_INT16_H_
#define TENSORFLOW_LITE_EXPERIMENTAL_MICROFRONTEND_LIB_KISS_FFT_INT16_H_

#include "tensorflow/lite/experimental/microfrontend/lib/kiss_fft_common.h"

#define FIXED_POINT 16
#include "kiss_fft.h"
#include "tools/kiss_fftr.h"
#undef FIXED_POINT
#undef kiss_fft_scalar

#endif  // TENSORFLOW_LITE_EXPERIMENTAL_MICROFRONTEND_LIB_KISS_FFT_INT16_H_
