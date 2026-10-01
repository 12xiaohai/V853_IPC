#include "yolov8_postprocess.h"

#include <math.h>
#include <stdlib.h>

/*
 * yolov8n.nb固定输出[1, 84, 2100]：4个框参数、80个COCO类别概率，
 * 2100来自320输入下的40x40、20x20和10x10三层候选框总数。
 */
#define YOLOV8_CANDIDATE_COUNT 2100
#define YOLOV8_ATTRIBUTE_COUNT 84
#define YOLOV8_CLASS_COUNT 80
#define YOLOV8_PERSON_CLASS 0
#define YOLOV8_MODEL_WIDTH 320.0f
#define YOLOV8_MODEL_HEIGHT 320.0f

static int compare_score_descending(const void *left, const void *right)
{
    const YoloDetection *a = left;
    const YoloDetection *b = right;

    if (a->score < b->score) {
        return 1;
    }
    if (a->score > b->score) {
        return -1;
    }
    return 0;
}

static int clamp_coordinate(int value, int minimum, int maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static float intersection_over_union(const YoloDetection *a,
                                     const YoloDetection *b)
{
    int left = a->xmin > b->xmin ? a->xmin : b->xmin;
    int top = a->ymin > b->ymin ? a->ymin : b->ymin;
    int right = a->xmax < b->xmax ? a->xmax : b->xmax;
    int bottom = a->ymax < b->ymax ? a->ymax : b->ymax;
    int intersection_width = right > left ? right - left : 0;
    int intersection_height = bottom > top ? bottom - top : 0;
    float intersection = (float)intersection_width * (float)intersection_height;
    float area_a = (float)(a->xmax - a->xmin) * (float)(a->ymax - a->ymin);
    float area_b = (float)(b->xmax - b->xmin) * (float)(b->ymax - b->ymin);
    float union_area = area_a + area_b - intersection;

    return union_area > 0.0f ? intersection / union_area : 0.0f;
}

int yolov8_decode_person(const float *output,
                         int image_width,
                         int image_height,
                         float confidence_threshold,
                         float nms_threshold,
                         YoloDetection *detections,
                         int detection_capacity)
{
    YoloDetection *proposals;
    float x_scale;
    float y_scale;
    int proposal_count = 0;
    int result_count = 0;
    int candidate;

    if (output == NULL || detections == NULL || detection_capacity <= 0 ||
        image_width <= 0 || image_height <= 0 ||
        confidence_threshold <= 0.0f || confidence_threshold >= 1.0f ||
        nms_threshold <= 0.0f || nms_threshold >= 1.0f) {
        return -1;
    }

    proposals = calloc(YOLOV8_CANDIDATE_COUNT, sizeof(*proposals));
    if (proposals == NULL) {
        return -1;
    }
    x_scale = (float)image_width / YOLOV8_MODEL_WIDTH;
    y_scale = (float)image_height / YOLOV8_MODEL_HEIGHT;

    for (candidate = 0; candidate < YOLOV8_CANDIDATE_COUNT; ++candidate) {
        int class_index;
        int best_class = 0;
        float best_score = output[4 * YOLOV8_CANDIDATE_COUNT + candidate];
        float center_x;
        float center_y;
        float width;
        float height;
        int xmin;
        int ymin;
        int xmax;
        int ymax;

        /* 与原项目一致：输出为NCW，先在同一个候选框的80类中取最大值。 */
        for (class_index = 1; class_index < YOLOV8_CLASS_COUNT; ++class_index) {
            float score = output[(4 + class_index) *
                                     YOLOV8_CANDIDATE_COUNT +
                                 candidate];
            if (score > best_score) {
                best_score = score;
                best_class = class_index;
            }
        }
        if (best_class != YOLOV8_PERSON_CLASS ||
            !isfinite(best_score) || best_score <= confidence_threshold) {
            continue;
        }

        center_x = output[candidate];
        center_y = output[YOLOV8_CANDIDATE_COUNT + candidate];
        width = output[2 * YOLOV8_CANDIDATE_COUNT + candidate];
        height = output[3 * YOLOV8_CANDIDATE_COUNT + candidate];
        if (!isfinite(center_x) || !isfinite(center_y) ||
            !isfinite(width) || !isfinite(height) ||
            width <= 0.0f || height <= 0.0f) {
            continue;
        }

        xmin = (int)((center_x - width * 0.5f) * x_scale);
        ymin = (int)((center_y - height * 0.5f) * y_scale);
        xmax = (int)((center_x + width * 0.5f) * x_scale);
        ymax = (int)((center_y + height * 0.5f) * y_scale);
        xmin = clamp_coordinate(xmin, 0, image_width - 1);
        ymin = clamp_coordinate(ymin, 0, image_height - 1);
        xmax = clamp_coordinate(xmax, 0, image_width);
        ymax = clamp_coordinate(ymax, 0, image_height);
        if (xmax <= xmin || ymax <= ymin) {
            continue;
        }

        proposals[proposal_count].label = YOLOV8_PERSON_CLASS;
        proposals[proposal_count].score = best_score;
        proposals[proposal_count].xmin = xmin;
        proposals[proposal_count].ymin = ymin;
        proposals[proposal_count].xmax = xmax;
        proposals[proposal_count].ymax = ymax;
        ++proposal_count;
    }

    qsort(proposals,
          (size_t)proposal_count,
          sizeof(*proposals),
          compare_score_descending);

    /* 按置信度从高到低执行NMS，删除与已保留框高度重叠的候选框。 */
    for (candidate = 0;
         candidate < proposal_count && result_count < detection_capacity;
         ++candidate) {
        int kept_index;
        int keep = 1;

        for (kept_index = 0; kept_index < result_count; ++kept_index) {
            if (intersection_over_union(&proposals[candidate],
                                        &detections[kept_index]) >
                nms_threshold) {
                keep = 0;
                break;
            }
        }
        if (keep) {
            detections[result_count++] = proposals[candidate];
        }
    }

    free(proposals);
    return result_count;
}
