/* C 化（2026-10-31，XiaoZhiCam）：去掉 namespace，改为直接包含 .c 源文件。
 * kiss_fft.c / tools/kiss_fftr.c **不单独编译**，只在这里被 include 一次，
 * 与上游做法一致（否则符号会重复定义）。 */

#include <stdint.h>

#include "tensorflow/lite/experimental/microfrontend/lib/kiss_fft_common.h"

#define FIXED_POINT 16
#include "kiss_fft.c"
#include "tools/kiss_fftr.c"
#undef FIXED_POINT
