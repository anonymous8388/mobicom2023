// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2018 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "net.h"

#include <math.h>
#if defined(USE_NCNN_SIMPLEOCV)
#include "simpleocv.h"
#else
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#endif
#include <stdio.h>

#include <iostream>
#include <random>
#include <vector>
#include <string>
#include <dirent.h>
#include <algorithm>
#include <random>


struct Object
{
    cv::Rect_<float> rect;
    int label;
    float prob;
};

static inline float intersection_area(const Object& a, const Object& b)
{
    cv::Rect_<float> inter = a.rect & b.rect;
    return inter.area();
}

static void qsort_descent_inplace(std::vector<Object>& objects, int left, int right)
{
    int i = left;
    int j = right;
    float p = objects[(left + right) / 2].prob;

    while (i <= j)
    {
        while (objects[i].prob > p)
            i++;

        while (objects[j].prob < p)
            j--;

        if (i <= j)
        {
            // swap
            std::swap(objects[i], objects[j]);

            i++;
            j--;
        }
    }

    #pragma omp parallel sections
    {
        #pragma omp section
        {
            if (left < j) qsort_descent_inplace(objects, left, j);
        }
        #pragma omp section
        {
            if (i < right) qsort_descent_inplace(objects, i, right);
        }
    }
}

static void qsort_descent_inplace(std::vector<Object>& objects)
{
    if (objects.empty())
        return;

    qsort_descent_inplace(objects, 0, objects.size() - 1);
}

static void nms_sorted_bboxes(const std::vector<Object>& objects, std::vector<int>& picked, float nms_threshold)
{
    picked.clear();

    const int n = objects.size();

    std::vector<float> areas(n);
    for (int i = 0; i < n; i++)
    {
        areas[i] = objects[i].rect.area();
    }

    for (int i = 0; i < n; i++)
    {
        const Object& a = objects[i];

        int keep = 1;
        for (int j = 0; j < (int)picked.size(); j++)
        {
            const Object& b = objects[picked[j]];

            // intersection over union
            float inter_area = intersection_area(a, b);
            float union_area = areas[i] + areas[picked[j]] - inter_area;
            //             float IoU = inter_area / union_area
            if (inter_area / union_area > nms_threshold)
                keep = 0;
        }

        if (keep)
            picked.push_back(i);
    }
}

static int detect_fasterrcnn(std::vector<std::string > files)
{
    ncnn::Net fasterrcnn;

    fasterrcnn.opt.use_vulkan_compute = false;
    fasterrcnn.opt.use_packing_layout = false;
    fasterrcnn.opt.use_reserved_3 = true;
    fasterrcnn.opt.use_reserved_8 = true;
    // original pretrained model from https://github.com/rbgirshick/py-faster-rcnn
    // py-faster-rcnn/models/pascal_voc/ZF/faster_rcnn_alt_opt/faster_rcnn_test.pt
    // https://dl.dropboxusercontent.com/s/o6ii098bu51d139/faster_rcnn_models.tgz?dl=0
    // ZF_faster_rcnn_final.caffemodel
    // the ncnn model https://github.com/nihui/ncnn-assets/tree/master/models
    fasterrcnn.load_param("../../examples/ZF_faster_rcnn_final.param");
    fasterrcnn.load_model("../../examples/ZF_faster_rcnn_final.bin");

    // hyper parameters taken from
    // py-faster-rcnn/lib/fast_rcnn/config.py
    // py-faster-rcnn/lib/fast_rcnn/test.py
    const int target_size = 600; // __C.TEST.SCALES


    for(auto name: files){
        const cv::Mat& bgr = cv::imread(name, 1);
            // scale to target detect size
        int w = bgr.cols;
        int h = bgr.rows;
        float scale = 1.f;
        if (w < h)
        {
            scale = (float)target_size / w;
            w = target_size;
            h = h * scale;
        }
        else
        {
            scale = (float)target_size / h;
            h = target_size;
            w = w * scale;
        }

        ncnn::Mat in = ncnn::Mat::from_pixels_resize(bgr.data, ncnn::Mat::PIXEL_BGR, bgr.cols, bgr.rows, w, h);

        const float mean_vals[3] = {102.9801f, 115.9465f, 122.7717f};
//        const float norm_vals[3] = {1.0 / 127.5, 1.0 / 127.5, 1.0 / 127.5};
//        in.substract_mean_normalize(mean_vals, norm_vals);
        in.substract_mean_normalize(mean_vals, 0);

        ncnn::Mat im_info(3);
        im_info[0] = h;
        im_info[1] = w;
        im_info[2] = scale;

        // step1, extract feature and all rois
        ncnn::Extractor ex1 = fasterrcnn.create_extractor();

        ex1.input("data", in);
        ex1.input("im_info", im_info);

        ncnn::Mat conv5_relu5; // feature
        ncnn::Mat rois;        // all rois
        ex1.extract("conv5_relu5", conv5_relu5);
        ex1.extract("rois", rois);

        fprintf(stderr, "\n");
    }


    return 0;
}

static void draw_objects(const cv::Mat& bgr, const std::vector<Object>& objects)
{
    static const char* class_names[] = {"background",
                                        "aeroplane", "bicycle", "bird", "boat",
                                        "bottle", "bus", "car", "cat", "chair",
                                        "cow", "diningtable", "dog", "horse",
                                        "motorbike", "person", "pottedplant",
                                        "sheep", "sofa", "train", "tvmonitor"
                                       };

    cv::Mat image = bgr.clone();

    for (size_t i = 0; i < objects.size(); i++)
    {
        const Object& obj = objects[i];

        fprintf(stderr, "%d = %.5f at %.2f %.2f %.2f x %.2f\n", obj.label, obj.prob,
                obj.rect.x, obj.rect.y, obj.rect.width, obj.rect.height);

        cv::rectangle(image, obj.rect, cv::Scalar(255, 0, 0));

        char text[256];
        sprintf(text, "%s %.1f%%", class_names[obj.label], obj.prob * 100);

        int baseLine = 0;
        cv::Size label_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseLine);

        int x = obj.rect.x;
        int y = obj.rect.y - label_size.height - baseLine;
        if (y < 0)
            y = 0;
        if (x + label_size.width > image.cols)
            x = image.cols - label_size.width;

        cv::rectangle(image, cv::Rect(cv::Point(x, y), cv::Size(label_size.width, label_size.height + baseLine)),
                      cv::Scalar(255, 255, 255), -1);

        cv::putText(image, text, cv::Point(x, y + label_size.height),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0));
    }

    cv::imshow("image", image);
    cv::waitKey(0);
}

std::vector<std::string> get_files(std::string PATH){
    struct dirent *ptr;
    DIR *dir;
    dir=opendir(PATH.c_str());
    std::vector<std::string> files;
    while((ptr=readdir(dir))!=NULL)
    {
        if(ptr->d_name[0] == '.')
            continue;

        files.push_back(PATH+ptr->d_name);
    }
    return files;
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s [imagepath]\n", argv[0]);
        return -1;
    }

    const char* imagepath = argv[1];

    std::vector<std::string> files = get_files(imagepath);

    std::shuffle (files.begin(), files.end(), std::mt19937(std::random_device()()));

    detect_fasterrcnn(files);

    return 0;
}

/** our
*/