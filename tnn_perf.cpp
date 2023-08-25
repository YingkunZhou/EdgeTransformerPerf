/* Reference code:
   https://github.com/Tencent/TNN/blob/master/doc/cn/user/api.md
   https://github.com/Tencent/TNN/blob/master/examples/base/tnn_sdk_sample.cc
   https://github.com/Tencent/TNN/blob/master/test/test.cc
*/

#include <iostream>
#include <numeric>
#include <string>
#include <vector>
#include <iomanip>
#include <filesystem>
#include <getopt.h>
#include <fstream>

#include <tnn/core/common.h>
#include <tnn/core/instance.h>
#include <tnn/core/macro.h>
#include <tnn/core/tnn.h>
#include "utils.h"

// Helper functions
std::string fdLoadFile(std::string path) {
    std::ifstream file(path);
    if (file.is_open()) {
        file.seekg(0, file.end);
        int size      = file.tellg();
        char* content = new char[size];
        file.seekg(0, file.beg);
        file.read(content, size);
        std::string fileContent;
        fileContent.assign(content, size);
        delete[] content;
        file.close();
        return fileContent;
    }

    return "";
}

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
    tnn::TNN &net,
    std::shared_ptr<tnn::Instance> &instance,
    std::vector<float> &input)
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

    std::shared_ptr<tnn::Mat> output_tensor = nullptr;
    tnn::Status status;

    std::vector<std::filesystem::path> classes = traverse_class(args.data_path);
    struct timespec start, end;
    clock_gettime(CLOCK_REALTIME, &start);
    for (const std::string& class_path : classes) {
        for (const auto & image: std::filesystem::directory_iterator(class_path)) {
            load_image(image.path(), input.data(), args.model, args.input_size, args.batch_size);
            auto input_tensor = std::make_shared<tnn::Mat>(tnn::DEVICE_NAIVE, tnn::NCHW_FLOAT, args.input_dims, input.data());
            status = instance->SetInputMat(input_tensor, tnn::MatConvertParam());
            status = instance->Forward();
            status = instance->GetOutputMat(output_tensor);
            num_predict++;
            bool acc1 = false;
            num_acc5 += acck((float *)output_tensor->GetData(), 5, class_index*scale+offset, acc1);
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
    tnn::TNN &net,
    std::shared_ptr<tnn::Instance> &instance,
    std::vector<float> &input)
{
    // Measure latency
    load_image("daisy.jpg", input.data(), args.model, args.input_size, args.batch_size);
    auto input_tensor = std::make_shared<tnn::Mat>(tnn::DEVICE_NAIVE, tnn::NCHW_FLOAT, args.input_dims, input.data());
    auto status = instance->SetInputMat(input_tensor, tnn::MatConvertParam());

    struct timespec start, end;
    clock_gettime(CLOCK_REALTIME, &end);
    clock_gettime(CLOCK_REALTIME, &start);
    /// warmup
#if !defined(DEBUG) && !defined(TEST)
    while (end.tv_sec - start.tv_sec < WARMUP_SEC) {
#endif
        status = instance->Forward();
        clock_gettime(CLOCK_REALTIME, &end);
#if !defined(DEBUG) && !defined(TEST)
    }
#endif

    std::shared_ptr<tnn::Mat> output_tensor = nullptr;
    status = instance->GetOutputMat(output_tensor);
    print_topk((float *)output_tensor->GetData(), 3);
#if defined(TEST)
    return;
#endif
    /// testup
    std::vector<double> time_list = {};
    double time_tot = 0;
    while (time_tot < TEST_SEC) {
        clock_gettime(CLOCK_REALTIME, &start);
        status = instance->Forward();
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
    char* arg_long = nullptr;
    char* only_test = nullptr;
    int num_threads = 1;

    static struct option long_options[] =
    {
        {"validation", no_argument, 0, 'v'},
        {"debug", no_argument, 0, 'g'},
        {"batch-size", required_argument, 0, 'b'},
        {"data-path",  required_argument, 0, 'd'},
        {"only-test",  required_argument, 0, 'o'},
        {"threads",  required_argument, 0, 't'},
        {"append",  required_argument, 0, 0},
        {0, 0, 0, 0}
    };
    int option_index;
    int c;
    while ((c = getopt_long(argc, argv, "vbdot", // TODO
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

    // TODO: Set the cpu affinity.
    // usually, -dl 0-3 for little core, -dl 4-7 for big core
    // only works when -dl flags were set. benchmark script not set -dl flags
    // SetCpuAffinity();

    for (const auto & model: test_models) {
        args.model = model.first;
        if (only_test && args.model.find(only_test) == std::string::npos) {
            continue;
        }

        args.input_size = model.second;

        std::cout << "Creating TNN net: " << args.model << std::endl;
        tnn::ModelConfig model_config;
        // model_config.model_type = tnn::MODEL_TYPE_NCNN;
        model_config.model_type = tnn::MODEL_TYPE_TNN;
        model_config.params.clear();
        // TODO: has opt suffix?
        std::string tnnproto = "tnn/" + args.model + ".opt.tnnproto";
        std::string tnnmodel = "tnn/" + args.model + ".opt.tnnmodel";

        model_config.params.push_back(fdLoadFile(tnnproto.c_str()));
        model_config.params.push_back(fdLoadFile(tnnmodel.c_str()));
        // model_config.params.push_back(model_path_str_) ??
        tnn::TNN net;
        auto status = net.Init(model_config);

        tnn::NetworkConfig network_config;
        network_config.device_type = tnn::DEVICE_ARM;
        // TODO: network_config.{library_path, precision, cache_path, network_type}
        args.input_dims = {1, 3/*image_channel*/, args.input_size, args.input_size};
        tnn::InputShapesMap input_shapes = {{"input", args.input_dims}};
        std::shared_ptr<tnn::Instance> instance = net.CreateInst(network_config, status, input_shapes);
        // TODO: num_threads <= 4 in orin doesn't work?!
        instance->SetCpuNumThreads(num_threads);

        size_t inputTensorSize  = vectorProduct(args.input_dims);
        std::vector<float> input_tensor(inputTensorSize);
        if (args.validation) {
            evaluate(net, instance, input_tensor);
        }
        else {
            benchmark(net, instance, input_tensor);
        }
    }
}
