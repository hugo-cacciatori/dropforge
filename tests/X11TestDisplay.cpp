#include "X11TestDisplay.h"
#include <X11/Xlib.h>
#include <cstdlib>
#include <iostream>
#include <initializer_list>

X11TestDisplay::X11TestDisplay()
{
    if (XInitThreads() == 0)
    {
        std::cerr << "FAIL: Xlib thread initialization\n";
        std::exit(1);
    }
    auto* display = XOpenDisplay(nullptr);
    if (display == nullptr)
    {
        std::cerr << "FAIL: GUI tests need an X11 display (use xvfb-run)\n";
        std::exit(1);
    }
    connection = display;
    // Pinned JUCE queries these with only_if_exists=true, then unconditionally
    // writes WM_PROTOCOLS and queries _NET_WM_STATE unconditionally. Bare Xvfb
    // has no window manager to create them. Prime the complete cached set so
    // creation, hidden/fullscreen queries and accessibility never use Atom 0.
    for (const auto* name : {"WM_PROTOCOLS", "WM_TAKE_FOCUS", "WM_DELETE_WINDOW",
                             "_NET_WM_PING", "WM_CHANGE_STATE", "WM_STATE", "_NET_WM_WINDOW_TYPE",
                             "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN", "_NET_WM_STATE_MAXIMIZED_HORZ",
                             "_NET_WM_STATE_MAXIMIZED_VERT"})
    {
        if (XInternAtom(display, name, False) == None)
        {
            std::cerr << "FAIL: cannot create X11 protocol " << name << '\n';
            std::exit(1);
        }
    }
    XSync(display, False);
}

X11TestDisplay::~X11TestDisplay()
{
    XCloseDisplay(static_cast<Display*>(connection));
}
