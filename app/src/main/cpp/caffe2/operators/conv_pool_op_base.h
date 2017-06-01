#ifndef CAFFE2_OPERATORS_CONV_POOL_OP_BASE_H_
#define CAFFE2_OPERATORS_CONV_POOL_OP_BASE_H_

#include "caffe2/core/context.h"
#include "caffe2/core/logging.h"
#include "caffe2/core/operator.h"
#include "caffe2/proto/caffe2_legacy.pb.h"
#include "caffe2/utils/math.h"

// This macro is here just to allow us to experiment with padding values that
// determines, when we have an odd number of pads, which side gets the one
// additional pad value, the head side, or the tail side. Setting it to false
// will enable the TensorFlow behavior, and setting it to true will enable
// a behavior more consistent with Caffe and CuDNN.
// This only affects the case when you set legacy pad to VALID or SAME. The
// behavior inherits from the early designs of Google's CNN implementation,
// where padding values are implicitly calculated instead of explicitly
// specified. This is still the case with TensorFlow. Many frameworks have
// followed a slightly different approach of explicitly giving padding values,
// in which case the value of this constant value does not matter.
const bool CAFFE2_PAD_HEAD_MORE = false;

namespace caffe2 {

template <class Context>
class ConvPoolOpBase : public Operator<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;
  ConvPoolOpBase(const OperatorDef& operator_def, Workspace* ws)
      : Operator<Context>(operator_def, ws),
        pad_(OperatorBase::GetSingleArgument<int>("pad", 0)),
        pad_t_(OperatorBase::GetSingleArgument<int>("pad_t", pad_)),
        pad_l_(OperatorBase::GetSingleArgument<int>("pad_l", pad_)),
        pad_b_(OperatorBase::GetSingleArgument<int>("pad_b", pad_)),
        pad_r_(OperatorBase::GetSingleArgument<int>("pad_r", pad_)),
        legacy_pad_(
            static_cast<LegacyPadding>(OperatorBase::GetSingleArgument<int>(
                "legacy_pad",
                LegacyPadding::NOTSET))),
        global_pooling_(
            OperatorBase::GetSingleArgument<int>("global_pooling", 0)),
        kernel_h_(OperatorBase::GetSingleArgument<int>(
            "kernel_h",
            OperatorBase::GetSingleArgument<int>("kernel", 0))),
        kernel_w_(OperatorBase::GetSingleArgument<int>(
            "kernel_w",
            OperatorBase::GetSingleArgument<int>("kernel", 0))),
        dilation_h_(OperatorBase::GetSingleArgument<int>(
            "dilation_h",
            OperatorBase::GetSingleArgument<int>("dilation", 1))),
        dilation_w_(OperatorBase::GetSingleArgument<int>(
            "dilation_w",
            OperatorBase::GetSingleArgument<int>("dilation", 1))),
        stride_h_(OperatorBase::GetSingleArgument<int>(
            "stride_h",
            OperatorBase::GetSingleArgument<int>("stride", 1))),
        stride_w_(OperatorBase::GetSingleArgument<int>(
            "stride_w",
            OperatorBase::GetSingleArgument<int>("stride", 1))),
        group_(OperatorBase::GetSingleArgument<int>("group", 1)),
        order_(StringToStorageOrder(
            OperatorBase::GetSingleArgument<string>("order", "NCHW"))),
        shared_buffer_(
            OperatorBase::GetSingleArgument<int>("shared_buffer", 0)),
        ws_(ws) {
    // For the padding, they should either be the legacy padding strategy
    // (VALID or SAME), or an explicit, non-negative value.
    if (legacy_pad_ == LegacyPadding::VALID ||
        legacy_pad_ == LegacyPadding::SAME) {
      CAFFE_ENFORCE(
          !OperatorBase::HasArgument("pad") &&
              !OperatorBase::HasArgument("pad_t") &&
              !OperatorBase::HasArgument("pad_l") &&
              !OperatorBase::HasArgument("pad_b") &&
              !OperatorBase::HasArgument("pad_r"),
          "If you use legacy padding VALID or SAME, you should not specify "
          "any specific padding values.");
    }
    CAFFE_ENFORCE(
        global_pooling_ == false ||
            (dilation_h_ == 1 && dilation_w_ == 1 && pad_ == 0 && pad_t_ == 0 &&
             pad_l_ == 0 && pad_b_ == 0 && pad_r_ == 0 && stride_h_ == 1 &&
             stride_w_ == 1),
        "If global_pooling is set, none of dilation/pad/stride should be set.");
    // Check kernel only if we are doing conv or pooling. The reason is that a
    // few other ops, like PadImage, are also using this base class. We really
    // need to clean this up.
    if (operator_def.name().find("Conv") == 0 ||
        operator_def.name().find("Pool") != std::string::npos) {
      CAFFE_ENFORCE(
          kernel_h_ && kernel_w_,
          "If you are doing convolution or pooling, you will need to set "
          "explicitly the kernel size.");
    }
    CAFFE_ENFORCE(dilation_h_ > 0);
    CAFFE_ENFORCE(dilation_w_ > 0);
    CAFFE_ENFORCE(pad_ >= 0);
    CAFFE_ENFORCE(pad_t_ >= 0);
    CAFFE_ENFORCE(pad_l_ >= 0);
    CAFFE_ENFORCE(pad_b_ >= 0);
    CAFFE_ENFORCE(pad_r_ >= 0);
    CAFFE_ENFORCE(stride_h_ > 0);
    CAFFE_ENFORCE(stride_w_ > 0);
    if (group_ != 1) {
      CAFFE_ENFORCE(
          dilation_h_ == 1 && dilation_w_ == 1,
          "When group is used, dilation should not be set at the same time.");
    }
  }

  // Sets the output size. The output channel is manually provided since
  // it may not be identical to the input channels.
  // This function can be used in the forward functions to obtain the output
  // sizes.
  // Note(jiayq): the templatization of this function is mainly to help
  // implementations that do not use first-class Tensor objects, such as the
  // MKL operator. One can still call this function with dummy
  // Tensor<CPUContext> objects in order to obtain the sizes.
  template <typename AlternativeContext>
  void SetOutputSize(
      const Tensor<AlternativeContext>& input,
      Tensor<AlternativeContext>* output,
      int output_channel) {
        CAFFE_ENFORCE(4 == input.ndim());
        CAFFE_ENFORCE(input.size() > 0);
        int output_height, output_width;
        int N = input.dim32(0);
        bool channel_first;

        InferOutputSize(
          input.dims(),
          output_channel,
          order_,
          global_pooling_,
          legacy_pad_,
          N,
          kernel_w_,
          kernel_h_,
          output_width,
          output_height,
          dilation_w_,
          dilation_h_,
          stride_w_,
          stride_h_,
          pad_t_,
          pad_b_,
          pad_l_,
          pad_r_,
          channel_first
        );
        if (channel_first) {
          output->Resize(N, output_channel, output_height, output_width);
        } else {
          output->Resize(N, output_height, output_width, output_channel);
        }
  }

  // Helper function that is also called from OperatorSchema. Modified
  // kernel parameters and output width/height, and channel_first.
  static inline void InferOutputSize(
      vector<TIndex> input_dims,
      int output_channel,
      StorageOrder order,
      bool global_pooling,
      LegacyPadding legacy_pad,
      int N,
      int& kernel_w,
      int& kernel_h,
      int& output_width,
      int& output_height,
      int dilation_w,
      int dilation_h,
      int stride_w,
      int stride_h,
      int pad_t,
      int pad_b,
      int pad_l,
      int pad_r,
      bool& channel_first
  ) {

    channel_first = false; // initialized to suppress compiler warning.
    int H = 0, W = 0; // initialized to suppress compiler warning.
    switch (order) {
      case StorageOrder::NHWC:
        channel_first = false;
        H = input_dims[1];
        W = input_dims[2];
        break;
      case StorageOrder::NCHW:
        // Old Caffe order.
        channel_first = true;
        H = input_dims[2];
        W = input_dims[3];
        break;
      default:
        CAFFE_THROW("Unknown Storage order: ", order);
    }

    output_height = 0, output_width = 0;
    if (global_pooling) {
      kernel_h = H;
      kernel_w = W;
      output_height = 1;
      output_width = 1;
    } else {
      ComputeSizeAndPad(
          H,
          stride_h,
          kernel_h,
          dilation_h,
          legacy_pad,
          &pad_t,
          &pad_b,
          &output_height);
      ComputeSizeAndPad(
          W,
          stride_w,
          kernel_w,
          dilation_w,
          legacy_pad,
          &pad_l,
          &pad_r,
          &output_width);
    }

  }

  // ComputePads could be used in backward functions to figure out the padding
  // values for the given input.
  void ComputePads(const int height, const int width) {
    if (global_pooling_) {
      kernel_h_ = height;
      kernel_w_ = width;
    } else if (legacy_pad_ != LegacyPadding::NOTSET) {
      int output_unused;
      ComputeSizeAndPad(
          height,
          stride_h_,
          kernel_h_,
          dilation_h_,
          legacy_pad_,
          &pad_t_,
          &pad_b_,
          &output_unused);
      ComputeSizeAndPad(
          width,
          stride_w_,
          kernel_w_,
          dilation_w_,
          legacy_pad_,
          &pad_l_,
          &pad_r_,
          &output_unused);
    }
  }

  bool RunOnDevice() override {
    CAFFE_ENFORCE(kernel_h_ > 0 || global_pooling_);
    CAFFE_ENFORCE(kernel_w_ > 0 || global_pooling_);
    switch (order_) {
      case StorageOrder::NHWC:
        // VLOG(2) << "Running NHWC";
        return RunOnDeviceWithOrderNHWC();
      case StorageOrder::NCHW:
        // VLOG(2) << "Running NCHW";
        return RunOnDeviceWithOrderNCHW();
      default:
        CAFFE_THROW("Unknown Storage order: ", order_);
    }
  }

  // The actual function that does the computation, if the different
  // storage order leads to different implementations.
  virtual bool RunOnDeviceWithOrderNHWC() {
    CAFFE_NOT_IMPLEMENTED;
  }
  virtual bool RunOnDeviceWithOrderNCHW() {
    CAFFE_NOT_IMPLEMENTED;
  }

  static vector<TensorShape> TensorInferenceForSchema(
      const OperatorDef& def,
      const vector<TensorShape>& in,
      int output_channel) {
    ArgumentHelper helper(def);
    int N = in[0].dims(0);
    int pad = helper.GetSingleArgument<int>("pad", 0);
    int output_width, output_height;
    bool channel_first;
    int kernel_h = helper.GetSingleArgument<int>(
        "kernel_h",
        helper.GetSingleArgument<int>("kernel", 1));
    int kernel_w = helper.GetSingleArgument<int>(
        "kernel_w",
        helper.GetSingleArgument<int>("kernel", 1));
    ConvPoolOpBase<CPUContext>::InferOutputSize(
      GetDimsVector(in[0]),
      output_channel,
      StringToStorageOrder(helper.GetSingleArgument<string>("order", "NCHW")),
      helper.GetSingleArgument<int>("global_pooling", 0),
      static_cast<LegacyPadding>(helper.GetSingleArgument<int>(
          "legacy_pad",
          LegacyPadding::NOTSET)),
      N,
      kernel_w,
      kernel_h,
      output_width,
      output_height,
      helper.GetSingleArgument<int>(
          "dilation_w",
          helper.GetSingleArgument<int>("dilation", 1)),
      helper.GetSingleArgument<int>(
          "dilation_h",
          helper.GetSingleArgument<int>("dilation", 1)),
      helper.GetSingleArgument<int>(
          "stride_w",
          helper.GetSingleArgument<int>("stride", 1)),
      helper.GetSingleArgument<int>(
          "stride_h",
          helper.GetSingleArgument<int>("stride", 1)),
      helper.GetSingleArgument<int>("pad_t", pad),
      helper.GetSingleArgument<int>("pad_b", pad),
      helper.GetSingleArgument<int>("pad_l", pad),
      helper.GetSingleArgument<int>("pad_r", pad),
      channel_first
    );
    vector<TensorShape> out(1);
    if (channel_first) {
      out[0] = CreateTensorShape(
          vector<int> {N, output_channel, output_height, output_width},
          TensorProto::FLOAT
      );
    } else {
      out[0] = CreateTensorShape(
          vector<int> {N, output_height, output_width, output_channel},
          TensorProto::FLOAT
      );
   }

   return out;
 }

 static vector<TensorShape> TensorInferenceForConv(
   const OperatorDef& def,
   const vector<TensorShape>& in) {
     return TensorInferenceForSchema(def, in, in[1].dims(0));
 }

 static vector<TensorShape> TensorInferenceForPool(
     const OperatorDef& def,
     const vector<TensorShape>& in) {
   ArgumentHelper helper(def);
   auto order =
       StringToStorageOrder(helper.GetSingleArgument<string>("order", "NCHW"));
   int num_channels =
       (order == StorageOrder::NCHW ? in[0].dims(1) : in[0].dims(3));
   return TensorInferenceForSchema(def, in, num_channels);
 }

 virtual ~ConvPoolOpBase() {}

private:
 // I put this private section before protected because these variables are
 // going to be initialized before pad_t_ et al. However, a derivative class
 // should never use these values. They should refer to pad_t et al. for the
 // exact padding values. This isolates out the padding scheme that are growing
 // unfortunately complex due to implementational differences from different
 // frameworks.
 int pad_;


protected:
 int pad_t_;
 int pad_l_;
 int pad_b_;
 int pad_r_;
 LegacyPadding legacy_pad_;
 bool global_pooling_;
 int kernel_h_;
 int kernel_w_;
 int dilation_h_;
 int dilation_w_;
 int stride_h_;
 int stride_w_;
 int group_;
 StorageOrder order_;
 bool shared_buffer_;
 Workspace* ws_;

 static inline void ComputeSizeAndPad(
     const int in_size,
     const int stride,
     const int kernel,
     const int dilation,
     LegacyPadding legacy_pad,
     int* pad_head,
     int* pad_tail,
     int* out_size) {
   const int dkernel = dilation * (kernel - 1) + 1;
   switch (legacy_pad) {
     case LegacyPadding::NOTSET:
       // We will just use the direct padding head and tail values, but we
       // will verify that they are non-negative.
       CAFFE_ENFORCE(*pad_head >= 0);
       CAFFE_ENFORCE(*pad_tail >= 0);
       CAFFE_ENFORCE(in_size + *pad_head + *pad_tail >= dkernel);
       *out_size = static_cast<int>(
           static_cast<float>(in_size + *pad_head + *pad_tail - dkernel) /
               stride +
           1);
       break;
     case LegacyPadding::VALID:
       *pad_head = 0;
       *pad_tail = 0;
       *out_size = (in_size - dkernel) / stride + 1;
       break;
     case LegacyPadding::SAME: {
       CAFFE_ENFORCE(
           1 == dilation, "Dilation not supported for legacy padding.");
       int legacy_target_size = (in_size + stride - 1) / stride;
       int pad_needed = (legacy_target_size - 1) * stride + kernel - in_size;
       if (CAFFE2_PAD_HEAD_MORE) {
         *pad_head = (pad_needed + 1) / 2;
       } else {
         *pad_head = pad_needed / 2;
       }
       *pad_tail = pad_needed - *pad_head;
       *out_size = (in_size + pad_needed - dkernel) / stride + 1;
       break;
     }
     case LegacyPadding::CAFFE_LEGACY_POOLING:
       // This is in order to adapt Caffe's pooling padding case. In this case,
       // we will only use pad_head and will compute pad_tail to match the
       // old caffe pooling strategy. Also see caffe2_legacy.proto for more
       // details.
       CAFFE_ENFORCE_GE(*pad_head, 0);
       // Here, notice that caffe casts UP while caffe2 casts DOWN for the
       // output size computation.
       *out_size = std::ceil(
           static_cast<float>(in_size + *pad_head * 2 - kernel) / stride + 1);
       // If we have padding, caffe also ensures that the last pooling starts
       // strictly inside the image (instead of at the padding); otherwise clip
       // the last.
       if (*pad_head > 0 && (*out_size - 1) * stride >= in_size + *pad_head) {
         --*out_size;
       }
       // Now, compare the output size with the standard Caffe2 output size.
       // The
       // caffe2 standard output size should always be no larger than the
       // output
       // size of caffe.
       int standard_out_size = static_cast<int>(
           static_cast<float>(in_size + *pad_head * 2 - kernel) / stride + 1);
       CAFFE_ENFORCE_GE(
           *out_size,
           standard_out_size,
           "This should never happen. If this happens, double check the logic "
           "above.");
       if (*out_size > standard_out_size) {
         LOG(WARNING)
             << "You are hitting a case where Caffe's legacy padding calculation "
                "is hit. This leads to inefficient and sometimes incorrect "
                "results. We are keeping this behavior for backward compatibility"
                ", but you are strongly recommended to move away from it.";
       }
       *pad_tail = *pad_head + stride * (*out_size - standard_out_size);
       break;
   }
  }

 private:
};

#define USE_CONV_POOL_BASE_FUNCTIONS(Context)     \
  USE_OPERATOR_FUNCTIONS(Context);                \
  using ConvPoolOpBase<Context>::pad_t_;          \
  using ConvPoolOpBase<Context>::pad_l_;          \
  using ConvPoolOpBase<Context>::pad_b_;          \
  using ConvPoolOpBase<Context>::pad_r_;          \
  using ConvPoolOpBase<Context>::legacy_pad_;     \
  using ConvPoolOpBase<Context>::global_pooling_; \
  using ConvPoolOpBase<Context>::kernel_h_;       \
  using ConvPoolOpBase<Context>::kernel_w_;       \
  using ConvPoolOpBase<Context>::dilation_h_;     \
  using ConvPoolOpBase<Context>::dilation_w_;     \
  using ConvPoolOpBase<Context>::stride_h_;       \
  using ConvPoolOpBase<Context>::stride_w_;       \
  using ConvPoolOpBase<Context>::group_;          \
  using ConvPoolOpBase<Context>::order_;          \
  using ConvPoolOpBase<Context>::shared_buffer_;  \
  using ConvPoolOpBase<Context>::ws_

} // namespace caffe2

#endif // CAFFE2_OPERATORS_CONV_POOL_OP_BASE_H_
