/* Reference code:
   https://github.com/PaddlePaddle/Paddle-Lite/blob/develop/lite/demo/cxx/mobile_classify/mobile_classify.cc
   https://github.com/PaddlePaddle/Paddle-Lite/blob/develop/lite/demo/cxx/mobile_light/mobilenetv1_light_api.cc
*/

#include <iostream>
#include <numeric>
#include <string>
#include <vector>
#include <iomanip>
#include <filesystem>
#include <getopt.h>
#include <fstream>

#include <paddle_api.h>
#include "utils.h"

const int WARMUP_SEC = 5;
const int TEST_SEC = 20;

struct {
  std::string model;
  bool validation;
  int input_size;
  int batch_size;
  bool debug;
  std::string data_path;
  std::vector<int> input_dims;
} args;

void evaluate(
    std::shared_ptr<paddle::lite_api::PaddlePredictor> &predictor,
    std::unique_ptr<paddle::lite_api::Tensor> &input_tensor)
{
    int class_index = 0;
    int num_predict = 0;
    int num_acc1 = 0;
    int num_acc5 = 0;
    std::cout << std::fixed << std::setprecision(4);

    int scale = 1;
    int offset = 0;
    if (args.data_path.find("20") != std::string::npos) {
        scale = 20;
    }
    else if (args.data_path.find("50") != std::string::npos) {
        scale = 50;
        offset = 15;
    }

    std::vector<std::filesystem::path> classes = traverse_class(args.data_path);
    struct timespec start, end;
    clock_gettime(CLOCK_REALTIME, &start);
    for (const std::string& class_path : classes) {
        for (const auto & image: std::filesystem::directory_iterator(class_path)) {
            load_image(image.path(), input_tensor->mutable_data<float>(), args.model, args.input_size, args.batch_size);
            predictor->Run();
            std::unique_ptr<const paddle::lite_api::Tensor> output_tensor(std::move(predictor->GetOutput(0)));
            num_predict++;
            bool acc1 = false;
            num_acc5 += acck(output_tensor->data<float>(), 5, class_index*scale+offset, acc1);
            num_acc1 += acc1;
        }
        class_index++;
        std::cout << "Done [" << class_index << "/" << classes.size() << "]";
        std::cout << "\tacc1: " << num_acc1*1.f/num_predict;
        std::cout << "\tacc5: " << num_acc5*1.f/num_predict << std::endl;
    }
    clock_gettime(CLOCK_REALTIME, &end);
    long long seconds = end.tv_sec - start.tv_sec;
    long long nanoseconds = end.tv_nsec - start.tv_nsec;
    double elapse = seconds + nanoseconds * 1e-9;
    std::cout << "elapse time: " << elapse << std::endl;
}

void benchmark(
    std::shared_ptr<paddle::lite_api::PaddlePredictor> &predictor,
    std::unique_ptr<paddle::lite_api::Tensor> &input_tensor)
{
    // Measure latency
    load_image("daisy.jpg", input_tensor->mutable_data<float>(), args.model, args.input_size, args.batch_size);

    struct timespec start, end;
    clock_gettime(CLOCK_REALTIME, &end);
    clock_gettime(CLOCK_REALTIME, &start);
    /// warmup
    while (end.tv_sec - start.tv_sec < WARMUP_SEC) {
        predictor->Run();
        clock_gettime(CLOCK_REALTIME, &end);
    }

    std::unique_ptr<const paddle::lite_api::Tensor> output_tensor(std::move(predictor->GetOutput(0)));
    print_topk(output_tensor->data<float>(), 3);
    /// testup
    std::vector<double> time_list = {};
    double time_tot = 0;
    while (time_tot < TEST_SEC) {
        clock_gettime(CLOCK_REALTIME, &start);
        predictor->Run();
        clock_gettime(CLOCK_REALTIME, &end);
        long long seconds = end.tv_sec - start.tv_sec;
        long long nanoseconds = end.tv_nsec - start.tv_nsec;
        double elapse = seconds + nanoseconds * 1e-9;
        time_list.push_back(elapse);
        time_tot += elapse;
    }

    double time_max = *std::max_element(time_list.begin(), time_list.end()) * 1000;
    double time_min = *std::min_element(time_list.begin(), time_list.end()) * 1000;
    double time_mean = time_tot * 1000 / time_list.size();
    std::sort(time_list.begin(), time_list.end());
    double time_median = time_list[time_list.size() / 2] * 1000;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "min =\t" << time_min << "ms\tmax =\t" << time_max << "ms\tmean =\t";
    std::cout << time_mean << "ms\tmedian =\t" << time_median << "ms" << std::endl;
}

int main(int argc, char* argv[])
{
    args.data_path = "imagenet-div50";
    args.validation = false;
    args.batch_size = 1;
    args.debug = false;
    bool use_opencl = false;
    char* arg_long = nullptr;
    char* only_test = nullptr;
    int num_threads = 1;

    static struct option long_options[] =
    {
        {"validation", no_argument, 0, 'v'},
        {"debug", no_argument, 0, 'g'},
        {"backend",  required_argument, 0, 'u'},
        {"batch-size", required_argument, 0, 'b'},
        {"data-path",  required_argument, 0, 'd'},
        {"only-test",  required_argument, 0, 'o'},
        {"threads",  required_argument, 0, 't'},
        {"append",  required_argument, 0, 0},
        {0, 0, 0, 0}
    };
    int option_index;
    int c;
    while ((c = getopt_long(argc, argv, "vgubdot", // TODO
            long_options, &option_index)) != -1)
    {
        switch (c)
        {
            case 0:
            {
                std::cout << "Got long option " << long_options[option_index].name << "." << std::endl;
                arg_long = optarg;
                if (arg_long)
                {
                    std::cout << arg_long << std::endl;
                }
                break;
            }
            case 'v':
                args.validation = true;
                break;
            case 'b':
                args.batch_size = atoi(optarg);
                break;
            case 'd':
                args.data_path = optarg;
                break;
            case 'o':
                only_test = optarg;
                break;
            case 'g':
                args.debug = true;
                break;
            case 'u':
                if (optarg[0] == 'o')
                    use_opencl = true;
                break;
            case 't':
                num_threads = atoi(optarg);
                break;
            case '?':
                std::cout << "Got unknown option." << std::endl;
                break;
            default:
                std::cout << "Got unknown parse returns: " << c << std::endl;
        }
    }
    // TODO:
    int power_mode = 0;

    for (const auto & model: test_models) {
        args.model = model.first;
        if (only_test && args.model.find(only_test) == std::string::npos) {
            continue;
        }

        args.input_size = model.second;

        std::cout << "Creating PaddlePredictor: " << args.model << std::endl;
        std::string model_file = "pdlite/" + args.model + ".nb";

        paddle::lite_api::MobileConfig config;
        // 1. Set MobileConfig
        config.set_model_from_file(model_file);

        if (use_opencl) {
            // NOTE: Use android gpu with opencl, you should ensure:
            //  first, [compile **cpu+opencl** paddlelite
            //    lib](https://github.com/PaddlePaddle/Paddle-Lite/blob/develop/docs/demo_guides/opencl.md);
            //  second, [convert and use opencl nb
            //    model](https://github.com/PaddlePaddle/Paddle-Lite/blob/develop/docs/user_guides/opt/opt_bin.md).

            bool is_opencl_backend_valid =
                ::paddle::lite_api::IsOpenCLBackendValid(/*check_fp16_valid = false*/);
            std::cout << "is_opencl_backend_valid:"
                        << (is_opencl_backend_valid ? "true" : "false") << std::endl;
            if (is_opencl_backend_valid) {
                // Set opencl kernel binary.
                // Large addtitional prepare time is cost due to algorithm selecting and
                // building kernel from source code.
                // Prepare time can be reduced dramitically after building algorithm file
                // and OpenCL kernel binary on the first running.
                // The 1st running time will be a bit longer due to the compiling time if
                // you don't call `set_opencl_binary_path_name` explicitly.
                // So call `set_opencl_binary_path_name` explicitly is strongly
                // recommended.

                // Make sure you have write permission of the binary path.
                // We strongly recommend each model has a unique binary name.
                const std::string bin_path = "pdlite/";
                const std::string bin_name = args.model + "_kernel.bin";
                config.set_opencl_binary_path_name(bin_path, bin_name);

                // opencl tune option
                // CL_TUNE_NONE: 0
                // CL_TUNE_RAPID: 1
                // CL_TUNE_NORMAL: 2
                // CL_TUNE_EXHAUSTIVE: 3
                const std::string tuned_path = "pdlite/";
                const std::string tuned_name = args.model + "_tuned.bin";
                config.set_opencl_tune(paddle::lite_api::CL_TUNE_NORMAL, tuned_path, tuned_name);

                // opencl precision option
                // CL_PRECISION_AUTO: 0, first fp16 if valid, default
                // CL_PRECISION_FP32: 1, force fp32
                // CL_PRECISION_FP16: 2, force fp16
                config.set_opencl_precision(paddle::lite_api::CL_PRECISION_FP16);
            } else {
                std::cout << "*** nb model will be running on cpu. ***" << std::endl;
                // you can give backup cpu nb model instead
                // config.set_model_from_file(cpu_nb_model_dir);
            }
        }

        config.set_threads(num_threads);
        config.set_power_mode(static_cast<paddle::lite_api::PowerMode>(power_mode));

        // 2. Create PaddlePredictor by MobileConfig
        std::shared_ptr<paddle::lite_api::PaddlePredictor> predictor =
            paddle::lite_api::CreatePaddlePredictor<paddle::lite_api::MobileConfig>(config);

        // 3. Prepare input data from image
        std::unique_ptr<paddle::lite_api::Tensor> input_tensor(std::move(predictor->GetInput(0)));
        input_tensor->Resize({1, 3, args.input_size, args.input_size});

        if (args.validation) {
            evaluate(predictor, input_tensor);
        }
        else {
            benchmark(predictor, input_tensor);
        }
    }
}
