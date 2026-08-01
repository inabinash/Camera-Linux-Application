#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/videodev2.h>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

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

struct Buffer {
    void* start = nullptr;
    size_t length = 0;
};

void unmap_buffers(std::vector<Buffer>& buffers) {
    for (Buffer& buffer : buffers) {
        if (buffer.start != nullptr) {
            munmap(buffer.start, buffer.length);
            buffer.start = nullptr;
        }
    }
}

void release_driver_buffers(int fd) {
    v4l2_requestbuffers request{};
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    request.count = 0;

    // A count of zero asks the driver to release its capture buffers.
    if (ioctl(fd, VIDIOC_REQBUFS, &request) == -1) {
        std::cerr << "Could not release camera buffers: " << std::strerror(errno) << '\n';
    }
}

bool setup_mmap_buffers(int fd, std::vector<Buffer>& buffers) {
    v4l2_requestbuffers request{};
    request.count = 4;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;

    // Ask the driver to allocate four capture buffers that we can map.
    if (ioctl(fd, VIDIOC_REQBUFS, &request) == -1) {
        std::cerr << "VIDIOC_REQBUFS failed: " << std::strerror(errno) << '\n';
        return false;
    }

    if (request.count == 0) {
        std::cerr << "The driver did not allocate any capture buffers.\n";
        return false;
    }

    std::cout << "\nDriver allocated " << request.count << " capture buffers:\n";
    buffers.resize(request.count);

    for (unsigned int index = 0; index < request.count; ++index) {
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;

        // Ask where this driver buffer is and how large it is.
        if (ioctl(fd, VIDIOC_QUERYBUF, &buffer) == -1) {
            std::cerr << "VIDIOC_QUERYBUF failed: " << std::strerror(errno) << '\n';
            unmap_buffers(buffers);
            release_driver_buffers(fd);
            return false;
        }

        buffers[index].length = buffer.length;
        buffers[index].start = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, buffer.m.offset);

        if (buffers[index].start == MAP_FAILED) {
            buffers[index].start = nullptr;
            std::cerr << "mmap failed: " << std::strerror(errno) << '\n';
            unmap_buffers(buffers);
            release_driver_buffers(fd);
            return false;
        }

        std::cout << "  Buffer " << index
                  << ": offset " << buffer.m.offset
                  << ", length " << buffer.length << " bytes\n";
    }

    return true;
}

bool queue_all_buffers(int fd, const std::vector<Buffer>& buffers) {
    for (unsigned int index = 0; index < buffers.size(); ++index) {
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;

        // Give this empty buffer to the driver so it can fill it with a frame.
        if (ioctl(fd, VIDIOC_QBUF, &buffer) == -1) {
            std::cerr << "VIDIOC_QBUF failed: " << std::strerror(errno) << '\n';
            return false;
        }
    }

    return true;
}

bool start_streaming(int fd) {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    // Tell the driver that queued buffers may now receive camera frames.
    if (ioctl(fd, VIDIOC_STREAMON, &type) == -1) {
        std::cerr << "VIDIOC_STREAMON failed: " << std::strerror(errno) << '\n';
        return false;
    }

    std::cout << "Streaming started.\n";
    return true;
}

bool dequeue_one_frame(int fd, v4l2_buffer& buffer) {
    pollfd poll_fd{};
    poll_fd.fd = fd;
    poll_fd.events = POLLIN;

    // The device was opened with O_NONBLOCK. Wait until a filled buffer is
    // ready instead of repeatedly calling VIDIOC_DQBUF too early.
    const int poll_result = poll(&poll_fd, 1, 2000);
    if (poll_result == -1) {
        std::cerr << "poll failed: " << std::strerror(errno) << '\n';
        return false;
    }

    if (poll_result == 0) {
        std::cerr << "Timed out waiting for a camera frame.\n";
        return false;
    }

    buffer = {};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;

    // Take one filled buffer back from the driver.
    if (ioctl(fd, VIDIOC_DQBUF, &buffer) == -1) {
        std::cerr << "VIDIOC_DQBUF failed: " << std::strerror(errno) << '\n';
        return false;
    }

    std::cout << "Captured one frame:\n";
    std::cout << "  Buffer index: " << buffer.index << '\n';
    std::cout << "  Bytes used: " << buffer.bytesused << '\n';
    std::cout << "  Sequence: " << buffer.sequence << '\n';
    return true;
}

bool requeue_buffer(int fd, v4l2_buffer& buffer) {
    // Return the buffer to the driver so it can be used for another frame.
    if (ioctl(fd, VIDIOC_QBUF, &buffer) == -1) {
        std::cerr << "VIDIOC_QBUF failed while re-queueing: "
                  << std::strerror(errno) << '\n';
        return false;
    }

    return true;
}

void stop_streaming(int fd) {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    // Stop capture before unmapping or releasing the buffers.
    if (ioctl(fd, VIDIOC_STREAMOFF, &type) == -1) {
        std::cerr << "VIDIOC_STREAMOFF failed: " << std::strerror(errno) << '\n';
        return;
    }

    std::cout << "Streaming stopped.\n";
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

    std::vector<Buffer> buffers;
    if (!setup_mmap_buffers(fd, buffers)) {
        close(fd);
        return 1;
    }

    if (!queue_all_buffers(fd, buffers)) {
        unmap_buffers(buffers);
        release_driver_buffers(fd);
        close(fd);
        return 1;
    }

    if (!start_streaming(fd)) {
        unmap_buffers(buffers);
        release_driver_buffers(fd);
        close(fd);
        return 1;
    }

    v4l2_buffer captured_buffer{};
    if (!dequeue_one_frame(fd, captured_buffer)) {
        stop_streaming(fd);
        unmap_buffers(buffers);
        release_driver_buffers(fd);
        close(fd);
        return 1;
    }

    if (!requeue_buffer(fd, captured_buffer)) {
        stop_streaming(fd);
        unmap_buffers(buffers);
        release_driver_buffers(fd);
        close(fd);
        return 1;
    }

    stop_streaming(fd);
    unmap_buffers(buffers);
    release_driver_buffers(fd);

    close(fd);
    std::cout << "Camera closed.\n";
    return 0;
}
