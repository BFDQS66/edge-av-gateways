#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: veg_generate_person_fixture input.jpg output.avi\n";
        return 1;
    }
    const cv::Mat source = cv::imread(argv[1]);
    if (source.empty()) {
        std::cerr << "cannot read input image\n";
        return 1;
    }
    cv::Mat frame;
    cv::resize(source, frame, {832, 416});
    cv::VideoWriter writer(argv[2], cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
                           10.0, frame.size());
    if (!writer.isOpened()) {
        std::cerr << "cannot create output video\n";
        return 1;
    }
    for (int i = 0; i < 12; ++i) writer.write(frame);
    return 0;
}
