// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vector>
#include "core/hle/service/cam/cam.h"
#include "core/hle/service/y2r_u.h"

namespace Camera {

/// An abstract class standing for a camera. All camera implementation should inherit from this.
class CameraInterface {
public:
    /// Starts the camera for video capturing.
    virtual void StartCapture() = 0;

    /// Stops the camera for video capturing.
    virtual void StopCapture() = 0;

    /**
     * Sets the video resolution from raw CAM service parameters.
     * For the meaning of the parameters, please refer to Service::CAM::Resolution. Note that the
     * actual camera implementation doesn't need to respect all the parameters. However, the width
     * and the height parameters must be respected and be used to determine the size of output
     * frames.
     * @param resolution The resolution parameters to set
     */
    virtual void SetResolution(const Service::CAM::Resolution& resolution) = 0;

    /**
     * Sets the vertical and horizontal flip applying on the frame.
     * @param flip Flip applying to the frame
     */
    virtual void SetFlip(Service::CAM::Flip flip) = 0;

    /**
     * Sets the effect applying on the frame.
     * @param effect Effect applying to the frame
     */
    virtual void SetEffect(Service::CAM::Effect effect) = 0;

    /**
     * Sets the output format of the frame.
     * @param format Output format of the frame
     */
    virtual void SetFormat(Service::CAM::OutputFormat format) = 0;

    /**
     * Receives a frame from the camera.
     * This function should be only called between a StartCapture call and a StopCapture call.
     * @returns a std::vector of u16, with one pixel per each, and with totally (width * height)
     *    pixels, where width and height are set by SetResolution
     */
    virtual std::vector<u16> ReceiveFrame() const = 0;

    virtual ~CameraInterface() = default;
};

} // namespace Camera
