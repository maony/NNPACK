#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <math.h>

#include <nnpack.h>
#include <nnpack/macros.h>
#include <nnpack/utils.h>

#include <nnpack/hwinfo.h>
#include <nnpack/validation.h>


struct NNP_CACHE_ALIGN relu_context {
	nnp_gradient_relu_function relu_function;
	const float* grad_output;
	const float* input;
	float* grad_input;
	float negative_slope;
};

static inline float grad_relu(float grad_output_data, float input_data, float negative_slope) {
	return signbit(input_data) ? grad_output_data * negative_slope : grad_output_data;
}

static void compute_relu_input_gradient(
	const struct relu_context context[restrict static 1],
	size_t block_start, size_t block_size)
{
	nnp_gradient_relu_function relu_function = context->relu_function;
	const float* grad_output                 = context->grad_output;
	const float* input                       = context->input;
	float* grad_input                        = context->grad_input;
	float negative_slope                     = context->negative_slope;

	relu_function(grad_output + block_start, input + block_start, grad_input + block_start, block_size, negative_slope);
}

enum nnp_status nnp_relu_input_gradient(
	size_t batch_size,
	size_t channels,
	const float grad_output[],
	const float input[],
	float grad_input[],
	float negative_slope,
	pthreadpool_t threadpool)
{
	enum nnp_status status = validate_relu_arguments(batch_size, channels);
	if (status != nnp_status_success) {
		return status;
	}

	size_t elements = batch_size * channels;
	const size_t simd_width = nnp_hwinfo.simd_width;

	assert(((uintptr_t) grad_output) % sizeof(float) == 0);
	assert(((uintptr_t) input) % sizeof(float) == 0);
	assert(((uintptr_t) grad_input) % sizeof(float) == 0);

	const size_t prologue_elements = min((size_t) (-(((uintptr_t) grad_input) / sizeof(float)) % simd_width), elements);
	for (size_t i = 0; i < prologue_elements; i++) {
		grad_input[i] = grad_relu(grad_output[i], input[i], negative_slope);
	}
	elements -= prologue_elements;
	grad_output += prologue_elements;
	input += prologue_elements;
	grad_input += prologue_elements;

	const size_t epilogue_elements = elements % simd_width;
	for (size_t i = 0; i < epilogue_elements; i++) {
		grad_input[elements - epilogue_elements + i] = grad_relu(
			grad_output[elements - epilogue_elements + i],
			input[elements - epilogue_elements + i],
			negative_slope);
	}
	elements -= epilogue_elements;

	struct relu_context relu_context = {
		.relu_function = nnp_hwinfo.activations.outplace_grad_relu,
		.grad_output = grad_output,
		.input = input,
		.grad_input = grad_input,
		.negative_slope = negative_slope,
	};

	pthreadpool_compute_1d_tiled(threadpool,
		(pthreadpool_function_1d_tiled_t) compute_relu_input_gradient,
		&relu_context,
		elements, round_down(nnp_hwinfo.blocking.l1 / sizeof(float), simd_width));

	return nnp_status_success;
}
