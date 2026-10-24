#pragma once

// Keep a separate connection alive while the GUI tests run so a bare Xvfb
// server retains the window-protocol atoms normally supplied by a desktop.
class X11TestDisplay
{
public:
    X11TestDisplay();
    ~X11TestDisplay();
    X11TestDisplay(const X11TestDisplay&) = delete;
    X11TestDisplay& operator=(const X11TestDisplay&) = delete;
private:
    void* connection = nullptr;
};
