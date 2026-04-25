#pragma once
#include <string>

namespace Luna
{

// Synchronous native OS file dialog (blocks the calling thread).
std::string OpenFileDialog(const char* title = "Open File",
                           const char* filter = "glTF Files\0*.gltf;*.glb\0All Files\0*.*\0");

// Non-blocking file dialog — opens on a background thread so the render loop
// keeps running.  Call OpenFileDialogAsync() once, then PollFileDialogResult()
// every frame.  Returns true when a result is ready (path may be empty if the
// user cancelled).
void OpenFileDialogAsync(const char* title = "Open File",
                         const char* filter = "glTF Files\0*.gltf;*.glb\0All Files\0*.*\0");
bool IsFileDialogOpen();
bool PollFileDialogResult(std::string& outPath);

} // namespace Luna

