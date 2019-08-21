// Copyright (C) 2018-2019 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

/**
 * @brief The entry point for inference engine Super Resolution demo application
 * @file super_resolution_demo/main.cpp
 * @example super_resolution_demo/main.cpp
 */
#include <algorithm>
#include <vector>
#include <string>
#include <memory>

#include <format_reader_ptr.h>
#include <inference_engine.hpp>
#ifdef WITH_EXTENSIONS
#include <ext_list.hpp>
#endif

#include <samples/slog.hpp>
#include <samples/args_helper.hpp>
#include <samples/ocv_common.hpp>

#include "super_resolution_demo.h"

using namespace InferenceEngine;

bool ParseAndCheckCommandLine(int argc, char *argv[]) {
    // ---------------------------Parsing and validation of input args--------------------------------------
    slog::info << "Parsing input parameters" << slog::endl;

    gflags::ParseCommandLineNonHelpFlags(&argc, &argv, true);
    if (FLAGS_h) {
        showUsage();
        showAvailableDevices();
        return false;
    }

    if (FLAGS_i.empty()) {
        throw std::logic_error("Parameter -i is not set");
    }

    if (FLAGS_m.empty()) {
        throw std::logic_error("Parameter -m is not set");
    }

    return true;
}


template <typename T>
void matToBlob(const cv::Mat& orig_image, InferenceEngine::Blob::Ptr& blob, int batchIndex = 0) {
    InferenceEngine::SizeVector blobSize = blob->getTensorDesc().getDims();
    const size_t width = blobSize[3];
    const size_t height = blobSize[2];
    const size_t channels = blobSize[1];

    T* blob_data = blob->buffer().as<T*>();
    int batchOffset = batchIndex * width * height * channels;

    if (channels == 1) {
        for (size_t  h = 0; h < height; h++) {
            for (size_t w = 0; w < width; w++) {
                blob_data[batchOffset + h * width + w] = orig_image.at<uchar>(h, w);
            }
        }
    } else if (channels == 3) {
        for (size_t c = 0; c < channels; c++) {
            for (size_t  h = 0; h < height; h++) {
                for (size_t w = 0; w < width; w++) {
                    blob_data[batchOffset + c * width * height + h * width + w] =
                            orig_image.at<cv::Vec3b>(h, w)[c];
                }
            }
        }
    } else {
        slog::err << "Unsupported number of channels" << slog::endl;
    }
}

int main(int argc, char *argv[]) {
    try {
        slog::info << "InferenceEngine: " << GetInferenceEngineVersion() << slog::endl;
        // ------------------------------ Parsing and validation of input args ---------------------------------
        if (!ParseAndCheckCommandLine(argc, argv)) {
            return 0;
        }

        /** This vector stores paths to the processed images **/
        std::vector<std::string> imageNames;
        parseInputFilesArguments(imageNames);
        if (imageNames.empty()) throw std::logic_error("No suitable images were found");
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 1. Load inference engine -------------------------------------
        slog::info << "Loading Inference Engine" << slog::endl;
        Core ie;

        /** Printing device version **/
        slog::info << "Device info: " << slog::endl;
        std::cout << ie.GetVersions(FLAGS_d) << std::endl;

#ifdef WITH_EXTENSIONS
        /** Loading default extensions **/
        if (FLAGS_d.find("CPU") != std::string::npos) {
            /**
             * cpu_extensions library is compiled from "extension" folder containing
             * custom MKLDNNPlugin layer implementations. These layers are not supported
             * by mkldnn, but they can be useful for inferring custom topologies.
            **/
            ie.AddExtension(std::make_shared<Extensions::Cpu::CpuExtensions>(), "CPU");
        }
#endif

        if (!FLAGS_l.empty()) {
            // CPU(MKLDNN) extensions are loaded as a shared library and passed as a pointer to base extension
            IExtensionPtr extension_ptr = make_so_pointer<IExtension>(FLAGS_l);
            ie.AddExtension(extension_ptr, "CPU");
            slog::info << "CPU Extension loaded: " << FLAGS_l << slog::endl;
        }
        if (!FLAGS_c.empty()) {
            // clDNN Extensions are loaded from an .xml description and OpenCL kernel files
            ie.SetConfig({{PluginConfigParams::KEY_CONFIG_FILE, FLAGS_c}}, "GPU");
            slog::info << "GPU Extension loaded: " << FLAGS_c << slog::endl;
        }
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 2. Read IR Generated by ModelOptimizer (.xml and .bin files) ------------
        slog::info << "Loading network files" << slog::endl;

        CNNNetReader networkReader;
        /** Read network model **/
        networkReader.ReadNetwork(FLAGS_m);

        /** Extract model name and load weights **/
        std::string binFileName = fileNameNoExt(FLAGS_m) + ".bin";
        networkReader.ReadWeights(binFileName);
        CNNNetwork network = networkReader.getNetwork();
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 3. Configure input & output ---------------------------------------------

        // --------------------------- Prepare input blobs -----------------------------------------------------
        slog::info << "Preparing input blobs" << slog::endl;

        /** Taking information about all topology inputs **/
        InputsDataMap inputInfo(network.getInputsInfo());

        if (inputInfo.size() != 1 && inputInfo.size() != 2)
            throw std::logic_error("The demo supports topologies with 1 or 2 inputs only");

        const std::string lrInputBlobName = "0";

        /** Collect images**/
        std::vector<cv::Mat> inputImages;
        for (const auto &i : imageNames) {
            cv::Mat img = cv::imread(i, -1);
            if (img.empty()) {
                slog::warn << "Image " + i + " cannot be read!" << slog::endl;
                continue;
            }

            /** Get size of low resolution input **/
            auto lrInputInfoItem = inputInfo[lrInputBlobName];
            int w = static_cast<int>(lrInputInfoItem->getTensorDesc().getDims()[3]);
            int h = static_cast<int>(lrInputInfoItem->getTensorDesc().getDims()[2]);
            int c = static_cast<int>(lrInputInfoItem->getTensorDesc().getDims()[1]);

            if (w != img.cols || h != img.rows) {
                slog::warn << "Size of the image " << i << " is not equal to WxH = " << w << "x" << h << slog::endl;
                continue;
            }
            if (c != img.channels()) {
              slog::warn << "Number of channels of the image " << i << " is not equal to " << c <<slog::endl;
              continue;
            }

            inputImages.push_back(img);
        }

        if (inputImages.empty()) throw std::logic_error("Valid input images were not found!");

        /** Setting batch size using image count **/
        network.setBatchSize(imageNames.size());
        slog::info << "Batch size is " << std::to_string(network.getBatchSize()) << slog::endl;

        // ------------------------------ Prepare output blobs -------------------------------------------------
        slog::info << "Preparing output blobs" << slog::endl;

        OutputsDataMap outputInfo(network.getOutputsInfo());
        // BlobMap outputBlobs;
        std::string firstOutputName;
        for (auto &item : outputInfo) {
            if (firstOutputName.empty()) {
                firstOutputName = item.first;
            }
            DataPtr outputData = item.second;
            if (!outputData) {
                throw std::logic_error("output data pointer is not valid");
            }

            item.second->setPrecision(Precision::FP32);
        }
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 4. Loading model to the device ------------------------------------------
        slog::info << "Loading model to the device" << slog::endl;
        ExecutableNetwork executableNetwork = ie.LoadNetwork(network, FLAGS_d);
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 5. Create infer request -------------------------------------------------
        slog::info << "Create infer request" << slog::endl;
        InferRequest inferRequest = executableNetwork.CreateInferRequest();
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 6. Prepare input --------------------------------------------------------
        Blob::Ptr lrInputBlob = inferRequest.GetBlob(lrInputBlobName);
        for (size_t i = 0; i < inputImages.size(); ++i) {
            cv::Mat img = inputImages[i];
            matToBlob<float_t>(img, lrInputBlob, i);

            bool twoInputs = inputInfo.size() == 2;
            if (twoInputs) {
                const std::string bicInputBlobName = "1";
                Blob::Ptr bicInputBlob = inferRequest.GetBlob(bicInputBlobName);

                int w = bicInputBlob->getTensorDesc().getDims()[3];
                int h = bicInputBlob->getTensorDesc().getDims()[2];

                cv::Mat resized;
                cv::resize(img, resized, cv::Size(w, h), 0, 0, cv::INTER_CUBIC);

                matToBlob<float_t>(resized, bicInputBlob, i);
            }
        }
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 7. Do inference ---------------------------------------------------------
        std::cout << "To close the application, press 'CTRL+C' here";
        if (FLAGS_show) {
            std::cout << " or switch to the output window and press any key";
        }
        std::cout << std::endl;

        slog::info << "Start inference" << slog::endl;
        inferRequest.Infer();
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 8. Process output -------------------------------------------------------
        const Blob::Ptr outputBlob = inferRequest.GetBlob(firstOutputName);
        const auto outputData = outputBlob->buffer().as<PrecisionTrait<Precision::FP32>::value_type*>();

        size_t numOfImages = outputBlob->getTensorDesc().getDims()[0];
        size_t numOfChannels = outputBlob->getTensorDesc().getDims()[1];
        size_t h = outputBlob->getTensorDesc().getDims()[2];
        size_t w = outputBlob->getTensorDesc().getDims()[3];
        size_t nunOfPixels = w * h;

        slog::info << "Output size [N,C,H,W]: " << numOfImages << ", " << numOfChannels << ", " << h << ", " << w << slog::endl;

        for (size_t i = 0; i < numOfImages; ++i) {
            std::vector<cv::Mat> imgPlanes;
            if (numOfChannels == 3) {
                imgPlanes = std::vector<cv::Mat>{
                      cv::Mat(h, w, CV_32FC1, &(outputData[i * nunOfPixels * numOfChannels])),
                      cv::Mat(h, w, CV_32FC1, &(outputData[i * nunOfPixels * numOfChannels + nunOfPixels])),
                      cv::Mat(h, w, CV_32FC1, &(outputData[i * nunOfPixels * numOfChannels + nunOfPixels * 2]))};
            } else {
                imgPlanes = std::vector<cv::Mat>{cv::Mat(h, w, CV_32FC1, &(outputData[i * nunOfPixels * numOfChannels]))};

                // Post-processing for text-image-super-resolution models
                cv::threshold(imgPlanes[0], imgPlanes[0], 0.5f, 1.0f, cv::THRESH_BINARY);
            };

            for (auto & img : imgPlanes)
                img.convertTo(img, CV_8UC1, 255);

            cv::Mat resultImg;
            cv::merge(imgPlanes, resultImg);

            if (FLAGS_show) {
                cv::imshow("result", resultImg);
                cv::waitKey();
            }

            std::string outImgName = std::string("sr_" + std::to_string(i + 1) + ".png");
            cv::imwrite(outImgName, resultImg);
        }
        // -----------------------------------------------------------------------------------------------------
    }
    catch (const std::exception &error) {
        slog::err << error.what() << slog::endl;
        return 1;
    }
    catch (...) {
        slog::err << "Unknown/internal exception happened" << slog::endl;
        return 1;
    }

    slog::info << "Execution successful" << slog::endl;
    slog::info << slog::endl << "This demo is an API example, for any performance measurements "
                                "please use the dedicated benchmark_app tool from the openVINO toolkit" << slog::endl;
    return 0;
}
