#include <jni.h>
#include <string>
#include <algorithm>
#define PROTOBUF_USE_DLLS 1
#include <caffe2/core/predictor.h>
#include <caffe2/core/operator.h>
#include <caffe2/core/timer.h>

#include "caffe2/core/init.h"

//#include <caffe2/utils/proto_utils.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include "classes.h"
#define IMG_H 227
#define IMG_W 227
#define IMG_C 3
#define MAX_DATA_SIZE IMG_H * IMG_W * IMG_C
#define alog(...) __android_log_print(ANDROID_LOG_ERROR, "F8DEMO", __VA_ARGS__);

static caffe2::NetDef _initNet, _predictNet;
static caffe2::Predictor *_predictor;
static char raw_data[MAX_DATA_SIZE];
static float input_data[MAX_DATA_SIZE];
static caffe2::Workspace ws;

//----------------------------------------------
#ifndef MAX
#define MAX(a, b) ({__typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#define MIN(a, b) ({__typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#endif

static const int kMaxChannelValue = 262143;


static inline uint32_t YUV2RGB(int nY, int nU, int nV) {
    nY -= 16;
    nU -= 128;
    nV -= 128;
    if (nY < 0) nY = 0;

    // This is the floating point equivalent. We do the conversion in integer
    // because some Android devices do not have floating point in hardware.
    // nR = (int)(1.164 * nY + 2.018 * nU);
    // nG = (int)(1.164 * nY - 0.813 * nV - 0.391 * nU);
    // nB = (int)(1.164 * nY + 1.596 * nV);

    int nR = (int)(1192 * nY + 1634 * nV);
    int nG = (int)(1192 * nY - 833 * nV - 400 * nU);
    int nB = (int)(1192 * nY + 2066 * nU);

    nR = MIN(kMaxChannelValue, MAX(0, nR));
    nG = MIN(kMaxChannelValue, MAX(0, nG));
    nB = MIN(kMaxChannelValue, MAX(0, nB));

    nR = (nR >> 10) & 0xff;
    nG = (nG >> 10) & 0xff;
    nB = (nB >> 10) & 0xff;

    return 0xff000000 | (nR << 16) | (nG << 8) | nB;
}

void ConvertYUV420ToARGB8888(const uint8_t* const yData,
                             const uint8_t* const uData,
                             const uint8_t* const vData, uint32_t* const output,
                             const int width, const int height,
                             const int y_row_stride, const int uv_row_stride,
                             const int uv_pixel_stride) {
    uint32_t* out = output;

    for (int y = 0; y < height; y++) {
        const uint8_t* pY = yData + y_row_stride * y;

        const int uv_row_start = uv_row_stride * (y >> 1);
        const uint8_t* pU = uData + uv_row_start;
        const uint8_t* pV = vData + uv_row_start;

        for (int x = 0; x < width; x++) {
            const int uv_offset = (x >> 1) * uv_pixel_stride;
            *out++ = YUV2RGB(pY[x], pU[uv_offset], pV[uv_offset]);
        }
    }
}

//------------------------------------------------------------------------------------------------------

// A function to load the NetDefs from protobufs.
void loadToNetDef(AAssetManager* mgr, caffe2::NetDef* net, const char *filename) {
    AAsset* asset = AAssetManager_open(mgr, filename, AASSET_MODE_BUFFER);
    assert(asset != nullptr);
    const void *data = AAsset_getBuffer(asset);
    assert(data != nullptr);
    off_t len = AAsset_getLength(asset);
    assert(len != 0);
    if (!net->ParseFromArray(data, len)) {
        alog("Couldn't parse net from data.\n");
    }
    AAsset_close(asset);
}
extern "C"
JNIEXPORT void JNICALL
Java_facebook_f8demo_ClassifyCamera_ConvertYUV420ToARGB8888(
        JNIEnv* env, jclass clazz, jbyteArray y, jbyteArray u, jbyteArray v,
        jintArray output, jint width, jint height, jint y_row_stride,
        jint uv_row_stride, jint uv_pixel_stride, jboolean halfSize) {
    jboolean inputCopy = JNI_FALSE;
    jbyte* const y_buff = env->GetByteArrayElements(y, &inputCopy);
    jboolean outputCopy = JNI_FALSE;
    jint* const o = env->GetIntArrayElements(output, &outputCopy);

    jbyte* const u_buff = env->GetByteArrayElements(u, &inputCopy);
    jbyte* const v_buff = env->GetByteArrayElements(v, &inputCopy);

    ConvertYUV420ToARGB8888(
            reinterpret_cast<uint8_t*>(y_buff), reinterpret_cast<uint8_t*>(u_buff),
            reinterpret_cast<uint8_t*>(v_buff), reinterpret_cast<uint32_t*>(o),
            width, height, y_row_stride, uv_row_stride, uv_pixel_stride);

    env->ReleaseByteArrayElements(u, u_buff, JNI_ABORT);
    env->ReleaseByteArrayElements(v, v_buff, JNI_ABORT);
    env->ReleaseByteArrayElements(y, y_buff, JNI_ABORT);
    env->ReleaseIntArrayElements(output, o, 0);
}
extern "C"
void
Java_facebook_f8demo_ClassifyCamera_initCaffe2(
        JNIEnv* env,
        jobject /* this */,
        jobject assetManager) {
    AAssetManager *mgr = AAssetManager_fromJava(env, assetManager);
    alog("Attempting to load protobuf netdefs...");
    loadToNetDef(mgr, &_initNet,   "squeeze_init_net.pb");
    loadToNetDef(mgr, &_predictNet,"squeeze_predict_net.pb");
    alog("done.");
    alog("Instantiating predictor...");
    _predictor = new caffe2::Predictor(_initNet, _predictNet);
    alog("done.")
}

float avg_fps = 0.0;
float total_fps = 0.0;
int iters_fps = 10;

extern "C"
JNIEXPORT jstring JNICALL
Java_facebook_f8demo_ClassifyCamera_classificationFromCaffe2(
        JNIEnv *env,
        jobject /* this */,
        jint h, jint w, jbyteArray Y, jbyteArray U, jbyteArray V,
        jint rowStride, jint pixelStride,
        jboolean infer_HWC) {
    if (!_predictor) {
        return env->NewStringUTF("Loading...");
    }
    jsize Y_len = env->GetArrayLength(Y);
    jbyte * Y_data = env->GetByteArrayElements(Y, 0);
    assert(Y_len <= MAX_DATA_SIZE);
    jsize U_len = env->GetArrayLength(U);
    jbyte * U_data = env->GetByteArrayElements(U, 0);
    assert(U_len <= MAX_DATA_SIZE);
    jsize V_len = env->GetArrayLength(V);
    jbyte * V_data = env->GetByteArrayElements(V, 0);
    assert(V_len <= MAX_DATA_SIZE);

#define min(a,b) ((a) > (b)) ? (b) : (a)
#define max(a,b) ((a) > (b)) ? (a) : (b)

    auto h_offset = max(0, (h - IMG_H) / 2);
    auto w_offset = max(0, (w - IMG_W) / 2);

    auto iter_h = IMG_H;
    auto iter_w = IMG_W;
    if (h < IMG_H) {
        iter_h = h;
    }
    if (w < IMG_W) {
        iter_w = w;
    }

    for (auto i = 0; i < iter_h; ++i) {
        jbyte* Y_row = &Y_data[(h_offset + i) * w];
        jbyte* U_row = &U_data[(h_offset + i) / 4 * rowStride];
        jbyte* V_row = &V_data[(h_offset + i) / 4 * rowStride];
        for (auto j = 0; j < iter_w; ++j) {
            // Tested on Pixel and S7.
            char y = Y_row[w_offset + j];
            char u = U_row[pixelStride * ((w_offset+j)/pixelStride)];
            char v = V_row[pixelStride * ((w_offset+j)/pixelStride)];

            float b_mean = 104.00698793f;
            float g_mean = 116.66876762f;
            float r_mean = 122.67891434f;

            auto b_i = 0 * IMG_H * IMG_W + j * IMG_W + i;
            auto g_i = 1 * IMG_H * IMG_W + j * IMG_W + i;
            auto r_i = 2 * IMG_H * IMG_W + j * IMG_W + i;

            if (infer_HWC) {
                b_i = (j * IMG_W + i) * IMG_C;
                g_i = (j * IMG_W + i) * IMG_C + 1;
                r_i = (j * IMG_W + i) * IMG_C + 2;
            }
/*
  R = Y + 1.402 (V-128)
  G = Y - 0.34414 (U-128) - 0.71414 (V-128)
  B = Y + 1.772 (U-V)
 */
            input_data[r_i] = -r_mean + (float) ((float) min(255., max(0., (float) (y + 1.402 * (v - 128)))));
            input_data[g_i] = -g_mean + (float) ((float) min(255., max(0., (float) (y - 0.34414 * (u - 128) - 0.71414 * (v - 128)))));
            input_data[b_i] = -b_mean + (float) ((float) min(255., max(0., (float) (y + 1.772 * (u - v)))));

        }
    }

    caffe2::TensorCPU input;
    if (infer_HWC) {
        input.Resize(std::vector<int>({IMG_H, IMG_W, IMG_C}));
    } else {
        input.Resize(std::vector<int>({1, IMG_C, IMG_H, IMG_W}));
    }
    memcpy(input.mutable_data<float>(), input_data, IMG_H * IMG_W * IMG_C * sizeof(float));
    caffe2::Predictor::TensorVector input_vec{&input};
    caffe2::Predictor::TensorVector output_vec;
    caffe2::Timer t;
    t.Start();
    _predictor->run(input_vec, &output_vec);
    float fps = 1000/t.MilliSeconds();
    total_fps += fps;
    avg_fps = total_fps / iters_fps;
    total_fps -= avg_fps;

    constexpr int k = 5;
    float max[k] = {0};
    int max_index[k] = {0};
    // Find the top-k results manually.
    if (output_vec.capacity() > 0) {
        for (auto output : output_vec) {
            for (auto i = 0; i < output->size(); ++i) {
                for (auto j = 0; j < k; ++j) {
                    if (output->template data<float>()[i] > max[j]) {
                        for (auto _j = k - 1; _j > j; --_j) {
                            max[_j - 1] = max[_j];
                            max_index[_j - 1] = max_index[_j];
                        }
                        max[j] = output->template data<float>()[i];
                        max_index[j] = i;
                        goto skip;
                    }
                }
                skip:;
            }
        }
    }
    std::ostringstream stringStream;
    stringStream << avg_fps << " FPS\n";

    for (auto j = 0; j < k; ++j) {
        stringStream << j << ": " << imagenet_classes[max_index[j]] << " - " << max[j] * 100 << "%\n";
    }
    return env->NewStringUTF(stringStream.str().c_str());
}
