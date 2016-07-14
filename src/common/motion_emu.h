// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

class EmuWindow;

namespace MotionEmu {

/**
 * Starts the motion emulation thread.
 * @param emu_window the EmuWindow to signal sensor state change
 */
void Init(EmuWindow& emu_window);

/**
 * Stops the motion emulation thread.
 */
void Shutdown();

/**
 * Signals the user begins tilting (e.g. the right button was pressed).
 * @param x the x-coordinate of the cursor
 * @param y the y-coordinate of the cursor
 */
void BeginTilt(int x, int y);

/**
 * Signals the user tilts (e.g. mouse was moved over the emu window).
 * @param x the x-coordinate of the cursor
 * @param y the y-coordinate of the cursor
 */
void Tilt(int x, int y);

/**
 * Signals the user stops tilting (e.g. the right button was released).
 */
void EndTilt();

}
