#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: veg_generate_fixture output.avi");
        const std::string output = argv[1];
        cv::VideoWriter writer(output, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
                               20.0, {640, 360});
        if (!writer.isOpened()) throw std::runtime_error("cannot create fixture video");
        for (int index = 0; index < 150; ++index) {
            cv::Mat frame(360, 640, CV_8UC3, cv::Scalar(22, 22, 22));
            const int x = 20 + 4 * index;
            cv::rectangle(frame, {x, 140, 36, 56}, {240, 240, 240}, cv::FILLED);
            writer.write(frame);
        }
        writer.release();
        std::cout << output << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
