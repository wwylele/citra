// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#ifdef __linux__

#include <errno.h>
#include <fcntl.h>

#include <cstring>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "core/frontend/camera/v4l2_camera.h"

namespace Camera {

void V4L2Camera::SetFormat(Service::CAM::OutputFormat output_format) {
    if (!is_valid)
        return;

    u32 v4l2_format;

    switch (output_format) {
    case Service::CAM::OutputFormat::YUV422:
        v4l2_format = V4L2_PIX_FMT_YUYV;
        break;
    case Service::CAM::OutputFormat::RGB565:
        v4l2_format = V4L2_PIX_FMT_RGB565;
        break;
    }

    format.fmt.pix.pixelformat = v4l2_format;

    if (ioctl(fd, VIDIOC_S_FMT, &format) == -1) {
        LOG_ERROR(Service_CAM, "VIDIOC_S_FMT: %s", std::strerror(errno));
    }

    // TODO(Link Mauve): use y2r:u instead when the user asks for RGB565, since the vast majority
    // of capture devices won’t give us that kind of format.
    if (format.fmt.pix.pixelformat != v4l2_format) {
        LOG_ERROR(Service_CAM, "Wrong pixel format, asked %08X got %08X!", v4l2_format,
                  format.fmt.pix.pixelformat);
    }
}

void V4L2Camera::SetFlip(Service::CAM::Flip flip) {
    if (!is_valid)
        return;

    using namespace Service::CAM;
    // TODO(wwylele): fallback if the capture device doesn't support flip.
    v4l2_control control;

    control.id = V4L2_CID_HFLIP;
    control.value = (flip == Flip::Horizontal) || (flip == Flip::Reverse);
    if (ioctl(fd, VIDIOC_S_CTRL, &control) == -1) {
        LOG_ERROR(Service_CAM, "VIDIOC_S_CTRL V4L2_CID_HFLIP: %s", std::strerror(errno));
    }

    control.id = V4L2_CID_VFLIP;
    control.value = (flip == Flip::Vertical) || (flip == Flip::Reverse);
    if (ioctl(fd, VIDIOC_S_CTRL, &control) == -1) {
        LOG_ERROR(Service_CAM, "VIDIOC_S_CTRL V4L2_CID_VFLIP: %s", std::strerror(errno));
    }
}

void V4L2Camera::SetEffect(Service::CAM::Effect effect) {
    if (!is_valid)
        return;

    if (effect != Service::CAM::Effect::None) {
        LOG_ERROR(Service_CAM, "Unimplemented effect %d", static_cast<int>(effect));
    }
}

void V4L2Camera::StartCapture() {
    if (!is_valid)
        return;

    if (is_capturing)
        return;

    v4l2_requestbuffers req{};
    v4l2_buffer buf;

    // We are using the mmap interface instead of the user-pointer one because the user could
    // change the target address at any frame.
    req.type = format_type;
    req.memory = V4L2_MEMORY_MMAP;
    req.count = NUM_BUFFERS;

    // Allocate our NUM_BUFFERS buffers.
    if (ioctl(fd, VIDIOC_REQBUFS, &req) == -1) {
        LOG_ERROR(Service_CAM, "VIDIOC_REQBUFS: %s", std::strerror(errno));
        return;
    }

    if (req.count < NUM_BUFFERS) {
        LOG_ERROR(Service_CAM, "wrong number of buffers!");
        return;
    }

    for (unsigned i = 0; i < NUM_BUFFERS; ++i) {
        std::memset(&buf, '\0', sizeof(buf));
        buf.type = format_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        // Query the characteristics of the newly-allocated buffer.
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) == -1) {
            LOG_ERROR(Service_CAM, "VIDIOC_QUERYBUF: %s", std::strerror(errno));
            return;
        }

        // Map the buffer so we can copy from this address whenever we get more data.
        mapped_buffers[i].memory =
            static_cast<char*>(mmap(nullptr, buf.length, PROT_READ, MAP_SHARED, fd, buf.m.offset));
        mapped_buffers[i].length = buf.length;

        // Enqueue the buffer to let the driver fill it whenever it gets an image.
        if (ioctl(fd, VIDIOC_QBUF, &buf) == -1) {
            LOG_ERROR(Service_CAM, "VIDIOC_QBUF: %s", std::strerror(errno));
            return;
        }
    }

    if (ioctl(fd, VIDIOC_STREAMON, &format_type) == -1) {
        LOG_ERROR(Service_CAM, "VIDIOC_STREAMON: %s", std::strerror(errno));
    }

    is_capturing = true;
}

void V4L2Camera::StopCapture() {
    if (!is_valid)
        return;

    if (!is_capturing)
        return;

    if (ioctl(fd, VIDIOC_STREAMOFF, &format_type) == -1) {
        LOG_ERROR(Service_CAM, "VIDIOC_STREAMOFF: %s", std::strerror(errno));
    }

    for (unsigned i = 0; i < NUM_BUFFERS; ++i) {
        if (mapped_buffers[i].memory)
            munmap(mapped_buffers[i].memory, mapped_buffers[i].length);
        mapped_buffers[i].memory = nullptr;
    }

    is_capturing = false;
}

void V4L2Camera::SetResolution(const Service::CAM::Resolution& resolution) {
    width = resolution.width;
    height = resolution.height;

    if (!is_valid)
        return;

    format.fmt.pix.width = width;
    format.fmt.pix.height = height;

    if (ioctl(fd, VIDIOC_S_FMT, &format) == -1) {
        LOG_ERROR(Service_CAM, "VIDIOC_S_FMT: %s", std::strerror(errno));
        return;
    }

    if (format.fmt.pix.width > width) {
        LOG_WARNING(Service_CAM, "Got a larger format than requested (%d > %d), will crop",
                    format.fmt.pix.width, width);
        crop_image_horizontally = true;
    } else {
        crop_image_horizontally = false;
    }

    if (format.fmt.pix.height > height)
        LOG_WARNING(Service_CAM, "Got a higher format than requested (%d > %d), will crop",
                    format.fmt.pix.height, height);
};

std::vector<u16> V4L2Camera::ReceiveFrame() const {
    std::vector<u16> buffer(width * height, 0);

    if (is_capturing) {
        v4l2_buffer buf{};
        buf.type = format_type;
        buf.memory = V4L2_MEMORY_MMAP;

        // Dequeue a buffer, at this point we can safely read from it.
        if (ioctl(fd, VIDIOC_DQBUF, &buf) == -1) {
            LOG_ERROR(Service_CAM, "VIDIOC_DQBUF: %s", std::strerror(errno));
            return buffer;
        }

        ASSERT(buf.flags & V4L2_BUF_FLAG_DONE);

        unsigned index = buf.index;
        ASSERT(index < NUM_BUFFERS);
        const char* memory = mapped_buffers[index].memory;

        // Not every capture device supports the size requested.
        if (crop_image_horizontally) {
            // There are always two bytes per pixel, both for YUYV and for RGB565.
            const unsigned bytes_per_pixel = 2;
            for (int y = 0; y < height; ++y)
                std::memcpy(buffer.data() + width * y,
                            static_cast<const char*>(memory) + (format.fmt.pix.bytesperline * y),
                            width * 2);
        } else {
            std::memcpy(buffer.data(), memory, width * height * 2);
        }

        // Enqueue back the buffer we just copied, so it will be filled again.
        if (ioctl(fd, VIDIOC_QBUF, &buf) == -1) {
            LOG_ERROR(Service_CAM, "VIDIOC_QBUF: %s", std::strerror(errno));
        }
    }

    return buffer;
}

V4L2Camera::V4L2Camera(int device_id) {
    std::string video_device = "/dev/video" + std::to_string(device_id);

    LOG_INFO(Service_CAM, "Open device %s", video_device.c_str());

    // Open the chosen v4l2 device.
    fd = open(video_device.c_str(), O_RDWR);
    if (fd < 0) {
        LOG_ERROR(Service_CAM, "failed to open v4l2 device");
        return;
    }

    v4l2_capability cap;

    // Query the capabilities of that device.
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {
        LOG_ERROR(Service_CAM, "VIDIOC_QUERYCAP: %s", std::strerror(errno));
        return;
    }

    // We only support single-planar video capture devices currently, which represent the vast
    // majority of camera devices.
    if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) {
        format_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    } else {
        LOG_ERROR(Service_CAM, "Not a video capture device!");
        return;
    }

    // We are using the streaming interface.
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        LOG_ERROR(Service_CAM, "Not a streaming device!");
        return;
    }

    // Retrieve the current format, which will be modified afterwards.
    std::memset(&format, '\0', sizeof(format));
    format.type = format_type;
    if (ioctl(fd, VIDIOC_G_FMT, &format) == -1) {
        LOG_ERROR(Service_CAM, "VIDIOC_G_FMT: %s", std::strerror(errno));
        return;
    }

    is_valid = true;

    LOG_INFO(Service_CAM, "V4L2 video capture initialized");
}

V4L2Camera::~V4L2Camera() {
    if (is_valid) {
        if (is_capturing) {
            StopCapture();
        }

        if (close(fd) == -1) {
            LOG_ERROR(Service_CAM, "close: %s", std::strerror(errno));
        }
    }

    LOG_INFO(Service_CAM, "V4L2 video capture finalized");
}

} // namespace Camera

#endif
