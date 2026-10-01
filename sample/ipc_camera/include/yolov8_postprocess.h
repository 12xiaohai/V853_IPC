#ifndef IPC_CAMERA_YOLOV8_POSTPROCESS_H
#define IPC_CAMERA_YOLOV8_POSTPROCESS_H

#define YOLOV8_MAX_DETECTIONS 100

typedef struct YoloDetection {
    int label;
    float score;
    int xmin;
    int ymin;
    int xmax;
    int ymax;
} YoloDetection;

/*
 * 解析本项目yolov8n.nb的输出张量，只返回COCO类别0（person）。
 * 返回检测数量；输入或输出异常时返回-1。
 */
int yolov8_decode_person(const float *output,
                         int image_width,
                         int image_height,
                         float confidence_threshold,
                         float nms_threshold,
                         YoloDetection *detections,
                         int detection_capacity);

#endif
