#pragma once

// Apple platforms: what goes to stderr (MoltenVK's errors, such as why a pipeline could not be made, and
// plume's) also goes to the log as "[stderr]" lines. On iOS nobody sees stderr otherwise (2026-11-25: a
// pipeline MoltenVK could not make crashed the game with no reason anywhere). Elsewhere it does nothing.
namespace StderrCapture
{
    void Start(); // once, after the logger
}
