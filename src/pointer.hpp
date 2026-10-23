// Optional: stop the mouse pointer wandering while three fingers swipe.
//
// The grab lives on its own X connection, confined to a 1x1 window at wherever
// the cursor is, and discards every pointer event. X drops the grab if the
// process dies, so a crash can never leave a frozen mouse behind.
#pragma once

#include <string>

namespace wintab {

class PointerLock {
public:
    PointerLock() = default;
    ~PointerLock();

    PointerLock(const PointerLock&) = delete;
    PointerLock& operator=(const PointerLock&) = delete;

    //: Confine the pointer to where it is now. Safe to call repeatedly.
    bool grab();
    void release();

    bool grabbed() const { return grabbed_; }
    const std::string& error() const { return error_; }

private:
    void* display_ = nullptr;  // Display*
    unsigned long window_ = 0;
    bool grabbed_ = false;
    std::string error_;
};

}  // namespace wintab
