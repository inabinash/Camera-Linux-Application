#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/videodev2.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

std::string fourcc_to_string(__u32 pixel_format) {
    std::string result;

    for (int i = 0; i < 4; ++i) {
        const char character = static_cast<char>((pixel_format >> (8 * i)) & 0xff);
        result += std::isprint(static_cast<unsigned char>(character)) ? character : '?';
    }

    return result;
}

void print_frame_intervals(int fd, __u32 pixel_format, __u32 width, __u32 height) {
    for (unsigned int index = 0;; ++index) {
        v4l2_frmivalenum interval{};
        interval.index = index;
        interval.pixel_format = pixel_format;
        interval.width = width;
        interval.height = height;

        // EINVAL means there are no more frame intervals to enumerate.
        if (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == -1) {
            break;
        }

        if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            const double fps = static_cast<double>(interval.discrete.denominator) /
                               interval.discrete.numerator;
            std::cout << " @ " << fps << " fps";
        }
    }
}

void print_supported_formats(int fd) {
    std::cout << "\nSupported formats:\n";

    for (unsigned int format_index = 0;; ++format_index) {
        v4l2_fmtdesc format{};
        format.index = format_index;
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        // EINVAL means there are no more pixel formats to enumerate.
        if (ioctl(fd, VIDIOC_ENUM_FMT, &format) == -1) {
            break;
        }

        std::cout << "- " << fourcc_to_string(format.pixelformat)
                  << " (" << format.description << ")\n";

        for (unsigned int size_index = 0;; ++size_index) {
            v4l2_frmsizeenum size{};
            size.index = size_index;
            size.pixel_format = format.pixelformat;

            // EINVAL means there are no more resolutions for this format.
            if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == -1) {
                break;
            }

            if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                const unsigned int width = size.discrete.width;
                const unsigned int height = size.discrete.height;

                std::cout << "  " << width << "x" << height;
                print_frame_intervals(fd, format.pixelformat, width, height);
                std::cout << '\n';
            } else if (size.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
                std::cout << "  " << size.stepwise.min_width << "x"
                          << size.stepwise.min_height << " to "
                          << size.stepwise.max_width << "x"
                          << size.stepwise.max_height << " (step "
                          << size.stepwise.step_width << "x"
                          << size.stepwise.step_height << ")\n";
            } else if (size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
                std::cout << "  " << size.stepwise.min_width << "x"
                          << size.stepwise.min_height << " to "
                          << size.stepwise.max_width << "x"
                          << size.stepwise.max_height << "\n";
            }
        }
    }
}

bool set_yuyv_format(int fd) {
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = 640;
    format.fmt.pix.height = 480;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    format.fmt.pix.field = V4L2_FIELD_NONE;

    // Ask the driver to use 640x480 YUYV frames. The driver may adjust this
    // request, so we read the values back from the same structure afterwards.
    if (ioctl(fd, VIDIOC_S_FMT, &format) == -1) {
        std::cerr << "VIDIOC_S_FMT failed: " << std::strerror(errno) << '\n';
        return false;
    }

    const v4l2_pix_format& pixels = format.fmt.pix;
    std::cout << "\nSelected format:\n";
    std::cout << "  " << pixels.width << "x" << pixels.height << '\n';
    std::cout << "  " << fourcc_to_string(pixels.pixelformat) << '\n';
    std::cout << "  Bytes per line: " << pixels.bytesperline << '\n';
    std::cout << "  Image size: " << pixels.sizeimage << " bytes\n";

    return true;
}

int main() {
    const char* device = "/dev/video0";

    std::cout << "Opening " << device << "...\n";

    // Open the V4L2 device. O_RDWR is needed later for camera configuration
    // and streaming; O_NONBLOCK will also be useful once we capture frames.
    const int fd = open(device, O_RDWR | O_NONBLOCK);
    if (fd == -1) {
        std::cerr << "Could not open " << device << ": " << std::strerror(errno) << '\n';
        return 1;
    }

    v4l2_capability cap{};

    // Ask the driver which capabilities this device has.
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {
        std::cerr << "VIDIOC_QUERYCAP failed: " << std::strerror(errno) << '\n';
        close(fd);
        return 1;
    }

    std::cout << "Camera: " << cap.card << '\n';
    std::cout << "Driver: " << cap.driver << '\n';
    std::cout << "Bus: " << cap.bus_info << '\n';

    // We need both of these capabilities for the streaming camera we will
    // build in later steps.
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        std::cerr << "This device does not support video capture.\n";
        close(fd);
        return 1;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        std::cerr << "This device does not support streaming I/O.\n";
        close(fd);
        return 1;
    }

    std::cout << "Video capture: supported\n";
    std::cout << "Streaming I/O: supported\n";

    if (!set_yuyv_format(fd)) {
        close(fd);
        return 1;
    }

    // Ask the driver which pixel formats and image sizes it can provide.
    print_supported_formats(fd);

    close(fd);
    std::cout << "Camera closed.\n";
    return 0;
}
